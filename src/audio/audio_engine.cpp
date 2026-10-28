#include "audio/audio_engine.h"
#include "common/hr.h"
#include "common/log.h"

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr uint32_t kRate = WasapiSource::kSampleRate;
constexpr uint32_t kCh = WasapiSource::kChannels;
constexpr uint32_t kChunk = AacEncoder::kFrameSamples;
// Расхождение больше 80 мс — это не дрейф, а разрыв потока (loopback молчит, когда ничего
// не играет; устройство подвисло): дыра заполняется тишиной, перекрытие обрезается.
constexpr int64_t kGapFrames = kRate * 8 / 100;
// Микшер отстаёт от «сейчас», чтобы пакеты WASAPI успели прийти (буфер устройства ~10–30 мс,
// у Bluetooth больше).
constexpr int64_t kMixLatencyFrames = kRate / 5;
constexpr int64_t kMaxGapFrames = kRate * 5;

// Регулятор дрейфа: ошибка уходит с постоянной времени ~2 с, растяжение не больше 0,5 %
// (≈ 9 центов по высоте тона — на слух незаметно, но покрывает дрейф до 5000 ppm).
constexpr double kDriftGain = 1.0 / (2.0 * kRate);
constexpr double kMaxStretch = 0.005;
// Сглаживание ошибки: метки QPC у пакетов дрожат на 1–2 мс.
constexpr double kErrSmoothing = 0.05;

} // namespace

void AudioEngine::Track::ResetResampler()
{
    hist.clear();
    phase = 0.0;
    errEma = 0.0;
    step = 1.0;
}

void AudioEngine::Track::Resample(const float* data, uint32_t frames)
{
    if (data)
        hist.insert(hist.end(), data, data + static_cast<size_t>(frames) * kCh);
    else
        hist.resize(hist.size() + static_cast<size_t>(frames) * kCh, 0.0f);

    const auto have = static_cast<int64_t>(hist.size() / kCh);
    // Линейная интерполяция между соседними входными кадрами; нужен кадр idx+1.
    while (true) {
        const auto idx = static_cast<int64_t>(phase);
        if (idx + 1 >= have)
            break;
        const auto f = static_cast<float>(phase - static_cast<double>(idx));
        const float* a = &hist[static_cast<size_t>(idx) * kCh];
        const float* b = a + kCh;
        for (uint32_t c = 0; c < kCh; ++c)
            buf.push_back(a[c] + (b[c] - a[c]) * f);
        phase += step;
    }

    const auto consumed = std::min(static_cast<int64_t>(phase), have);
    hist.erase(hist.begin(), hist.begin() + static_cast<ptrdiff_t>(consumed * kCh));
    phase -= static_cast<double>(consumed);
}

void AudioEngine::Track::Write(int64_t pos, bool hasPos, const float* data, uint32_t frames)
{
    if (!primed) {
        start = pos;
        primed = true;
    }

    if (hasPos) {
        // Куда ляжет первый кадр пакета: конец buf + то, что ещё ждёт в ресемплере.
        const double landing = static_cast<double>(End()) + (static_cast<double>(hist.size() / kCh) - phase) / step;
        const double err = landing - static_cast<double>(pos);

        if (err < -static_cast<double>(kGapFrames)) {
            const auto gap = static_cast<int64_t>(-err);
            if (gap >= kMaxGapFrames) {
                buf.clear();
                start = pos;
            } else {
                buf.resize(buf.size() + static_cast<size_t>(gap) * kCh, 0.0f);
            }
            ResetResampler();
        } else if (err > static_cast<double>(kGapFrames)) {
            const auto skip = static_cast<int64_t>(err);
            ResetResampler();
            if (skip >= frames)
                return;
            if (data)
                data += skip * kCh;
            frames -= static_cast<uint32_t>(skip);
        } else {
            errEma += kErrSmoothing * (err - errEma);
            step = 1.0 + std::clamp(errEma * kDriftGain, -kMaxStretch, kMaxStretch);
        }
    }

    Resample(data, frames);
}

void AudioEngine::Track::MixInto(int64_t pos, uint32_t frames, float* out, float gain) const
{
    const int64_t from = std::max(pos, start);
    const int64_t to = std::min(pos + frames, End());
    for (int64_t f = from; f < to; ++f) {
        const float* s = &buf[static_cast<size_t>(f - start) * kCh];
        float* d = &out[static_cast<size_t>(f - pos) * kCh];
        for (uint32_t c = 0; c < kCh; ++c)
            d[c] += s[c] * gain;
    }
}

void AudioEngine::Track::DiscardBefore(int64_t pos)
{
    if (pos <= start)
        return;
    if (pos >= End()) {
        buf.clear();
        start = pos;
        return;
    }
    buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>((pos - start) * kCh));
    start = pos;
}

AudioEngine::AudioEngine(const MediaClock& clock, const AudioOptions& opt, PacketFn out)
    : clock_(clock), out_(std::move(out))
{
    systemGain_ = opt.systemGain;
    micGain_ = opt.micGain;
    auto add = [&](WasapiSource::Kind kind, const char* label) {
        try {
            Track t;
            t.source = std::make_unique<WasapiSource>(
                kind, kind == WasapiSource::Kind::SystemLoopback ? opt.renderDeviceId : opt.captureDeviceId);
            t.mic = kind == WasapiSource::Kind::Microphone;
            LogInfo("Аудио: %s — %s\n", label, t.source->Name().c_str());
            tracks_.push_back(std::move(t));
        } catch (const std::exception& e) {
            LogInfo("Аудио: %s недоступен (%s)\n", label, e.what());
        }
    };
    if (opt.systemAudio)
        add(WasapiSource::Kind::SystemLoopback, "системный звук");
    if (opt.microphone)
        add(WasapiSource::Kind::Microphone, "микрофон");
    if (tracks_.empty())
        throw std::runtime_error("нет ни одного аудиоисточника");

    encoder_ = std::make_unique<AacEncoder>(kRate, kCh, opt.bitrateKbps);
}

AudioEngine::~AudioEngine()
{
    try {
        Stop();
    } catch (...) {
    }
}

int64_t AudioEngine::FramePosFromPts(int64_t pts) const
{
    return MulDiv64(pts, kRate, kTimescale);
}

void AudioEngine::Start()
{
    if (running_)
        return;
    mixPos_ = FramePosFromPts(clock_.NowPts());
    for (auto& t : tracks_)
        t.source->Start();
    running_ = true;
    thread_ = std::thread(&AudioEngine::ThreadMain, this);
}

void AudioEngine::Stop()
{
    if (!thread_.joinable())
        return;
    running_ = false;
    thread_.join();
}

void AudioEngine::ThreadMain()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    try {
        while (running_) {
            PollSources();
            MixReady(false);
            Sleep(5);
        }
        PollSources();
        MixReady(true);
        encoder_->Drain(out_);
    } catch (const std::exception& e) {
        LogError("Аудиопоток остановлен: %s\n", e.what());
    } catch (const winrt::hresult_error& e) {
        LogError("Аудиопоток остановлен: 0x%08X\n", static_cast<unsigned>(e.code()));
    }
    for (auto& t : tracks_)
        t.source->Stop();
    winrt::uninit_apartment();
}

void AudioEngine::PollSources()
{
    for (auto& t : tracks_) {
        t.source->Drain([&](int64_t qpcHns, const float* data, uint32_t frames) {
            const bool hasPos = qpcHns != WasapiSource::kNoTimestamp;
            const int64_t pos = hasPos ? FramePosFromPts(clock_.PtsFromQpcHns(qpcHns)) : (t.primed ? t.End() : mixPos_);
            t.Write(pos, hasPos, data, frames);
        });
    }
}

void AudioEngine::MixReady(bool flushAll)
{
    const int64_t limit = flushAll ? FramePosFromPts(clock_.NowPts())
                                   : FramePosFromPts(clock_.NowPts()) - kMixLatencyFrames;
    std::vector<float> mix(static_cast<size_t>(kChunk) * kCh);
    std::vector<int16_t> pcm(mix.size());

    while (mixPos_ + kChunk <= limit) {
        std::fill(mix.begin(), mix.end(), 0.0f);
        for (auto& t : tracks_) {
            t.MixInto(mixPos_, kChunk, mix.data(), (t.mic ? micGain_ : systemGain_).load(std::memory_order_relaxed));
            t.DiscardBefore(mixPos_ + kChunk);
        }
        for (size_t i = 0; i < mix.size(); ++i)
            pcm[i] = static_cast<int16_t>(std::lrint(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f));

        encoder_->Encode(pcm.data(), kChunk, MulDiv64(mixPos_, kTimescale, kRate), out_);
        mixPos_ += kChunk;
    }
}
