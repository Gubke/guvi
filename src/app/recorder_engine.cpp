#include "app/recorder_engine.h"

#include "app/background_jobs.h"
#include "app/hotkeys.h"
#include "app/overlay_hud.h"
#include "app/recording_session.h"
#include "app/replay_buffer.h"
#include "app/screenshot.h"
#include "audio/audio_engine.h"
#include "capture/scaler.h"
#include "capture/webcam_compositor.h"
#include "capture/wgc_capture.h"
#include "common/clock.h"
#include "common/d3d_device.h"
#include "common/hr.h"
#include "common/log.h"
#include "common/mf_scope.h"
#include "common/packet_queue.h"
#include "mux/muxer.h"

#include <dwmapi.h>
#include <timeapi.h>

#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <optional>

namespace {

std::wstring MakeOutputPath(const std::wstring& dir, const wchar_t* prefix, const wchar_t* ext)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    // Записи за день: guvi_YYYYMMDD_1.mp4, номер растёт с единицы.
    if (prefix && wcscmp(ext, L"mp4") == 0 && (wcscmp(prefix, L"rec") == 0 || wcscmp(prefix, L"live") == 0 ||
                                               wcscmp(prefix, L"replay") == 0)) {
        const auto stem = std::format(L"guvi_{:04}{:02}{:02}_", t.wYear, t.wMonth, t.wDay);
        int maxN = 0;
        std::error_code ec;
        for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
            if (ec || !ent.is_regular_file())
                continue;
            const auto name = ent.path().filename().wstring();
            if (name.size() <= stem.size() + 4 || _wcsnicmp(name.c_str(), stem.c_str(), stem.size()) != 0)
                continue;
            if (_wcsicmp(ent.path().extension().c_str(), L".mp4") != 0)
                continue;
            int n = 0;
            bool digits = false;
            for (size_t i = stem.size(); i < name.size() && name[i] != L'.'; ++i) {
                if (name[i] < L'0' || name[i] > L'9') {
                    digits = false;
                    break;
                }
                digits = true;
                n = n * 10 + (name[i] - L'0');
            }
            if (digits && n > maxN)
                maxN = n;
        }
        return (std::filesystem::path(dir) / std::format(L"{}{}.mp4", stem, maxN + 1)).wstring();
    }
    const auto name = std::format(L"guvi_{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}.{}", t.wYear, t.wMonth, t.wDay, t.wHour,
                                  t.wMinute, t.wSecond, t.wMilliseconds, ext);
    return (std::filesystem::path(dir) / name).wstring();
}

void WriteClip(const std::wstring& path, const StreamSetup& setup, const std::vector<PacketPtr>& packets)
{
    int64_t base = -1;
    int64_t last = 0;
    for (const auto& p : packets) {
        if (p->stream == StreamKind::Video) {
            if (base < 0)
                base = p->pts;
            last = p->pts + p->duration;
        }
    }
    if (base < 0)
        return;

    auto muxer = CreateMuxer(path, setup, base);
    for (const auto& p : packets)
        muxer->Write(*p);
    muxer->Finish();
    LogInfo("★ Повтор сохранён: %s (%.1f с)", WideToUtf8(path).c_str(), static_cast<double>(last - base) / kTimescale);
}

WgcCapture::GraphicsCaptureItem CreateItem(const SourceSpec& s)
{
    switch (s.type) {
    case SourceSpec::Type::Window:
        return WgcCapture::ItemForHwnd(s.hwnd);
    case SourceSpec::Type::WindowTitle:
        return WgcCapture::ItemForWindow(s.title);
    default:
        return WgcCapture::ItemForMonitor(s.monitor);
    }
}

GpuVendor VendorForPref(EncoderPref p)
{
    switch (p) {
    case EncoderPref::Nvenc: return GpuVendor::Nvidia;
    case EncoderPref::Amf: return GpuVendor::Amd;
    case EncoderPref::Qsv: return GpuVendor::Intel;
    default: return GpuVendor::Unknown;
    }
}

// Окно ещё нельзя отдать в Windows.Graphics.Capture — сессию не создаём.
class SourceNotReady : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Пустая строка — окно на экране и его можно захватывать.
std::string WindowStateHint(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return "Окно закрыто — захват продолжится, когда оно снова откроется";
    if (IsIconic(hwnd))
        return "Окно свёрнуто — захват начнётся, когда его развернут";
    if (!IsWindowVisible(hwnd))
        return "Окно ещё не видно — ждём, когда оно появится";
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return "Окно ещё не видно — ждём, когда оно появится";
    return {};
}

HMONITOR HudMonitor(const SourceSpec& s)
{
    switch (s.type) {
    case SourceSpec::Type::Monitor:
        return MonitorByIndex(s.monitor);
    case SourceSpec::Type::Window:
        return IsWindow(s.hwnd) ? MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTOPRIMARY) : nullptr;
    default:
        return nullptr;
    }
}

struct TimerPeriod {
    TimerPeriod() { timeBeginPeriod(1); }
    ~TimerPeriod() { timeEndPeriod(1); }
};

// Очередь кодирования. Цикл кадров только копирует готовый кадр в свободную текстуру пула и ставит
// (текстура, pts) в очередь; NVENC / AMF / MF работают в своём потоке. Поэтому ожидание кодировщика
// (GPU занят игрой) не сдвигает захват, а один кадр можно поставить несколько раз — дубли для ровного CFR.
class EncodeQueue {
public:
    static constexpr int kPool = 4;

    EncodeQueue(const D3DContext& d3d, IVideoEncoder& encoder, uint32_t width, uint32_t height, size_t maxJobs,
                IVideoEncoder::PacketSink sink)
        : d3d_(d3d), encoder_(encoder), maxJobs_(maxJobs), sink_(std::move(sink))
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (auto& t : pool_)
            ThrowIfFailed(d3d_.device->CreateTexture2D(&desc, nullptr, t.put()), "CreateTexture2D(encode pool)");
        thread_ = std::thread(&EncodeQueue::Worker, this);
    }

    ~EncodeQueue() { Close(); }

    // Копирует stage в свободную текстуру и ставит её на кодирование для каждого pts из списка
    // (первый — с forceIdr). Возвращает, сколько pts поставлено; остальное — пропуск (очередь полна).
    size_t Submit(ID3D11Texture2D* stage, const int64_t* pts, size_t count, bool forceIdr)
    {
        int slot = -1;
        size_t n = 0;
        {
            std::lock_guard lock(mtx_);
            if (closed_ || failed_)
                return 0;
            n = std::min(count, maxJobs_ - std::min(maxJobs_, jobs_.size()));
            for (int i = 0; i < kPool && n > 0; ++i)
                if (refs_[i] == 0) {
                    slot = i;
                    break;
                }
            if (slot < 0 || n == 0)
                return 0;
            refs_[slot] = static_cast<int>(n);  // занята, пока не скопирована во вход кодировщика
        }
        d3d_.context->CopyResource(pool_[slot].get(), stage);
        {
            std::lock_guard lock(mtx_);
            for (size_t i = 0; i < n; ++i)
                jobs_.push_back({ slot, pts[i], forceIdr && i == 0 });
        }
        cv_.notify_one();
        return n;
    }

    // Докодировать всё поставленное и остановить поток.
    void Close()
    {
        {
            std::lock_guard lock(mtx_);
            closed_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    bool Failed() const
    {
        std::lock_guard lock(mtx_);
        return failed_;
    }
    std::string Error() const
    {
        std::lock_guard lock(mtx_);
        return error_;
    }
    size_t Depth() const
    {
        std::lock_guard lock(mtx_);
        return jobs_.size();
    }

private:
    struct Job {
        int slot;
        int64_t pts;
        bool forceIdr;
    };

    void Worker()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        for (;;) {
            Job job{};
            {
                std::unique_lock lock(mtx_);
                cv_.wait(lock, [&] { return closed_ || !jobs_.empty(); });
                if (jobs_.empty())
                    break;
                job = jobs_.front();
                jobs_.pop_front();
            }
            try {
                d3d_.context->CopyResource(encoder_.InputTexture(), pool_[job.slot].get());
                {
                    std::lock_guard lock(mtx_);
                    --refs_[job.slot];
                }
                encoder_.Encode(job.pts, job.forceIdr, sink_);
            } catch (const std::exception& e) {
                std::lock_guard lock(mtx_);
                failed_ = true;
                error_ = std::string("кодировщик: ") + e.what();
                jobs_.clear();
                break;
            }
        }
        winrt::uninit_apartment();
    }

    const D3DContext& d3d_;
    IVideoEncoder& encoder_;
    const size_t maxJobs_;
    IVideoEncoder::PacketSink sink_;
    std::array<winrt::com_ptr<ID3D11Texture2D>, kPool> pool_;
    int refs_[kPool]{};

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    bool closed_ = false;
    bool failed_ = false;
    std::string error_;
    std::thread thread_;
};

// Трансляция: stage (уже с веб-камерой) → масштаб в свою текстуру → второй кодировщик в своей
// очереди → RTMP-поток. Локальная запись и захват её не ждут ни на кодировании, ни на сети.
struct LiveStream {
    StreamConfig cfg;
    VideoStreamInfo info;
    std::unique_ptr<IVideoEncoder> encoder;
    winrt::com_ptr<ID3D11Texture2D> tex;
    std::unique_ptr<Scaler> scaler;
    std::unique_ptr<OverlayCompositor> overlays;
    std::shared_ptr<RtmpPublisher> publisher;
    std::unique_ptr<EncodeQueue> queue;  // объявлена последней — разрушается первой
    int64_t startQpc = 0;
    int64_t basePts = 0;
    int64_t frame = 0;
    bool needIdr = true;
    bool startedRecording = false;
    bool haveFrame = false;
};

} // namespace

RecorderEngine::RecorderEngine(EngineConfig cfg)
    : cfg_(std::move(cfg))
{
    systemGain_ = cfg_.systemGain;
    micGain_ = cfg_.micGain;
    hotkeyBinds_ = cfg_.hotkeyBinds;
}

RecorderEngine::~RecorderEngine()
{
    RequestStop();
    Join();
}

void RecorderEngine::RequestStop()
{
    stop_ = true;
}

void RecorderEngine::Join()
{
    if (thread_.joinable())
        thread_.join();
}

void RecorderEngine::StartThread()
{
    {
        std::lock_guard lock(statusMtx_);
        status_ = {};
        status_.running = true;
    }
    thread_ = std::thread([this] {
        std::string error;
        try {
            Run();
        } catch (const winrt::hresult_error& e) {
            error = std::format("WinRT 0x{:08X}: {}", static_cast<unsigned>(e.code()), WideToUtf8(e.message()));
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!error.empty())
            LogError("Движок остановлен: %s", error.c_str());
        {
            std::lock_guard lock(statusMtx_);
            status_.running = false;
            status_.ready = false;
            status_.recording = false;
            status_.stopping = false;
            status_.error = error;
        }
        winrt::uninit_apartment();
    });
}

void RecorderEngine::SetWebcam(std::shared_ptr<WebcamSource> source)
{
    std::lock_guard lock(webcamMtx_);
    webcam_ = std::move(source);
}

void RecorderEngine::SetWebcamLayout(const WebcamLayout& layout)
{
    std::lock_guard lock(webcamMtx_);
    webcamLayout_ = layout;
}

void RecorderEngine::SetHotkeyBinds(const HotkeyBinds& binds)
{
    std::lock_guard lock(hotkeyMtx_);
    hotkeyBinds_ = binds;
    hotkeysDirty_ = true;
}

void RecorderEngine::SetStreamOverlays(std::vector<OverlaySprite> overlays)
{
    if (overlays.size() > static_cast<size_t>(OverlayCompositor::kMax))
        overlays.resize(OverlayCompositor::kMax);
    std::lock_guard lock(overlayMtx_);
    overlays_ = std::move(overlays);
}

EngineStatus RecorderEngine::Status() const
{
    std::lock_guard lock(statusMtx_);
    return status_;
}

void RecorderEngine::Publish(const EngineStatus& s)
{
    std::lock_guard lock(statusMtx_);
    status_ = s;
}

void RecorderEngine::SetSource(const SourceSpec& source)
{
    std::lock_guard lock(sourceMtx_);
    pendingSource_ = source;
    hasPendingSource_ = true;
}

bool RecorderEngine::PullSource()
{
    std::lock_guard lock(sourceMtx_);
    if (!hasPendingSource_)
        return false;
    cfg_.source = pendingSource_;
    hasPendingSource_ = false;
    return true;
}

void RecorderEngine::LatchHotkeys(Hotkeys* keys)
{
    if (!keys)
        return;
    if (keys->Take(HotkeyAction::ToggleRecord))
        cmdToggle_ = true;
    if (keys->Take(HotkeyAction::SaveReplay))
        cmdReplay_ = true;
    if (keys->Take(HotkeyAction::Screenshot))
        cmdShot_ = true;
    if (keys->Take(HotkeyAction::ToggleStream))
        cmdStream_ = true;
    if (keys->Take(HotkeyAction::TogglePause))
        cmdPause_ = true;
}

bool RecorderEngine::WaitForWindow(const std::string& why)
{
    if (stop_.load())
        return false;
    if (PullSource() && cfg_.source.type != SourceSpec::Type::Window)
        return true;
    if (cfg_.source.type != SourceSpec::Type::Window)
        throw std::runtime_error(why);

    std::string hint = why.empty() ? WindowStateHint(cfg_.source.hwnd) : why;
    if (hint.empty())
        return true;

    const auto publish = [&](const std::string& text) {
        EngineStatus st = Status();
        st.running = true;
        st.ready = false;
        st.recording = false;
        st.paused = false;
        st.stopping = false;
        st.error.clear();
        st.sourceHint = text;
        Publish(st);
    };
    publish(hint);

    std::unique_ptr<Hotkeys> keys;
    while (!stop_.load()) {
        if (PullSource()) {
            if (cfg_.source.type != SourceSpec::Type::Window)
                return true;
            hint = WindowStateHint(cfg_.source.hwnd);
            if (hint.empty())
                return true;
            publish(hint);
        } else {
            const std::string now = WindowStateHint(cfg_.source.hwnd);
            if (now.empty())
                return true;
            if (now != hint) {
                hint = now;
                publish(hint);
            }
        }
        if (cfg_.hotkeys && !keys) {
            HotkeyBinds binds = cfg_.hotkeyBinds;
            {
                std::lock_guard lock(hotkeyMtx_);
                if (hotkeysDirty_)
                    binds = hotkeyBinds_;
            }
            keys = std::make_unique<Hotkeys>(binds, true, true, true, true, true);
        }
        if (keys) {
            keys->PumpUntil(QpcNow() + QpcFrequency() / 10);
            LatchHotkeys(keys.get());
        } else {
            Sleep(100);
        }
    }
    LatchHotkeys(keys.get());
    return false;
}

void RecorderEngine::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    MfScope mf;

    if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported())
        throw std::runtime_error("Windows.Graphics.Capture не поддерживается (нужна Windows 10 1903+)");

    for (;;) {
        if (stop_.load())
            return;
        try {
            if (!RunOnce())
                return;
        } catch (const SourceNotReady& e) {
            if (!WaitForWindow(e.what()))
                return;
        }
    }
}

bool RecorderEngine::RunOnce()
{
    PullSource();
    if (stop_.load())
        return false;
    if (cfg_.source.type == SourceSpec::Type::Window) {
        const std::string why = WindowStateHint(cfg_.source.hwnd);
        if (!why.empty())
            throw SourceNotReady(why);
    }

    const D3DContext d3d = CreateD3DContext(VendorForPref(cfg_.encoder));
    WgcCapture::GraphicsCaptureItem item{ nullptr };
    try {
        item = CreateItem(cfg_.source);
    } catch (const winrt::hresult_error&) {
        if (cfg_.source.type != SourceSpec::Type::Window)
            throw;
        std::string why = WindowStateHint(cfg_.source.hwnd);
        if (why.empty())
            why = "Окно ещё не видно — ждём, когда оно появится";
        throw SourceNotReady(why);
    }
    if (cfg_.source.type == SourceSpec::Type::Window) {
        const auto sz = item.Size();
        if (sz.Width < 16 || sz.Height < 16)
            throw SourceNotReady("Окно ещё не видно — ждём, когда оно появится");
    }
    WgcCapture capture(d3d, std::move(item), cfg_.cursor);

    StreamSetup setup;
    setup.video.codec = cfg_.codec;
    setup.video.width = capture.Width();
    setup.video.height = capture.Height();
    if (cfg_.outputHeight > 0 && cfg_.outputHeight < capture.Height()) {
        setup.video.height = cfg_.outputHeight & ~1u;
        setup.video.width = static_cast<uint32_t>(std::llround(static_cast<double>(capture.Width()) *
                                                               setup.video.height / capture.Height())) & ~1u;
    }
    setup.video.fps = cfg_.fps;
    setup.video.bitrateKbps = cfg_.bitrateKbps;
    setup.video.keyintSeconds = cfg_.keyintSeconds;
    setup.video.lowLatency = cfg_.lowLatency;

    const std::unique_ptr<IVideoEncoder> encoderPtr = CreateVideoEncoder(d3d, setup.video, cfg_.encoder);
    IVideoEncoder& encoder = *encoderPtr;
    setup.videoSequenceHeader = encoder.SequenceHeader();

    // Кадр собирается в stage (захват → масштаб → веб-камера), оттуда — в очередь кодирования.
    winrt::com_ptr<ID3D11Texture2D> stage;
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = setup.video.width;
        desc.Height = setup.video.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(d3d.device->CreateTexture2D(&desc, nullptr, stage.put()), "CreateTexture2D(stage)");
    }

    // Разрешение меньше источника: кадр захвата → промежуточная текстура → масштаб в stage.
    winrt::com_ptr<ID3D11Texture2D> scaledSource;
    std::unique_ptr<Scaler> scaler;
    if (setup.video.width != capture.Width() || setup.video.height != capture.Height()) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = capture.Width();
        desc.Height = capture.Height();
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(d3d.device->CreateTexture2D(&desc, nullptr, scaledSource.put()), "CreateTexture2D(scale src)");
        scaler = std::make_unique<Scaler>(d3d.device.get(), scaledSource.get(), stage.get());
        LogInfo("Масштаб: %ux%u → %ux%u", capture.Width(), capture.Height(), setup.video.width, setup.video.height);
    }
    // Веб-камера рисуется в stage после масштаба — она есть в записи, повторах и скриншотах.
    // Кадр камеры никогда не ждём: нет нового — рисуется последний полученный.
    std::unique_ptr<WebcamCompositor> compositor;
    try {
        compositor = std::make_unique<WebcamCompositor>(d3d.device.get(), stage.get());
    } catch (const std::exception& e) {
        LogWarn("Веб-камера в записи недоступна: %s", e.what());
    }
    const WebcamSource* lastCam = nullptr;
    const auto grabFrame = [&]() {
        if (!scaler) {
            if (!capture.CopyLatest(stage.get()))
                return false;
        } else {
            if (!capture.CopyLatest(scaledSource.get()))
                return false;
            scaler->Draw(d3d.context.get());
        }
        if (compositor) {
            std::shared_ptr<WebcamSource> cam;
            WebcamLayout layout;
            {
                std::lock_guard lock(webcamMtx_);
                cam = webcam_;
                layout = webcamLayout_;
            }
            if (cam.get() != lastCam) {
                compositor->Reset();
                lastCam = cam.get();
            }
            if (cam) {
                compositor->Update(d3d.context.get(), *cam);
                compositor->Draw(d3d.context.get(), layout);
            }
        }
        return true;
    };

    const int64_t freq = QpcFrequency();
    const MediaClock clock(QpcNow());

    PacketQueue audioQueue;
    std::unique_ptr<AudioEngine> audio;
    if (cfg_.systemAudio || cfg_.microphone) {
        AudioOptions opt;
        opt.systemAudio = cfg_.systemAudio;
        opt.microphone = cfg_.microphone;
        opt.micGain = cfg_.micGain;
        opt.systemGain = cfg_.systemGain;
        opt.bitrateKbps = cfg_.audioBitrateKbps;
        opt.renderDeviceId = cfg_.renderDeviceId;
        opt.captureDeviceId = cfg_.captureDeviceId;
        try {
            audio = std::make_unique<AudioEngine>(clock, opt, [&audioQueue](PacketPtr p) { audioQueue.Push(std::move(p)); });
            setup.audio = audio->Info();
        } catch (const std::exception& e) {
            LogWarn("Аудио выключено: %s", e.what());
        } catch (const winrt::hresult_error& e) {
            LogWarn("Аудио выключено: 0x%08X", static_cast<unsigned>(e.code()));
        }
    }

    {
        std::error_code ec;
        std::filesystem::create_directories(cfg_.outDir, ec);
    }

    std::optional<ReplayBuffer> replay;
    if (cfg_.replaySeconds > 0) {
        replay.emplace(static_cast<int64_t>(cfg_.replaySeconds) * kTimescale);
        LogInfo("Replay-буфер: последние %u с", cfg_.replaySeconds);
    }

    std::unique_ptr<Hotkeys> hotkeys;
    if (cfg_.hotkeys) {
        LogInfo("Горячие клавиши:");
        hotkeys = std::make_unique<Hotkeys>(cfg_.hotkeyBinds, true, replay.has_value(), true, true, true);
    }

    capture.Start();
    const bool windowSrc = cfg_.source.type == SourceSpec::Type::Window;
    if (windowSrc) {
        // Не роняем поток: кнопка REC и клавиша остаются нажатыми, пока не придёт кадр.
        EngineStatus wait{};
        wait.running = true;
        wait.sourceHint = "Нет кадра от окна — ждём";
        Publish(wait);
        while (!grabFrame()) {
            if (stop_.load())
                return false;
            if (PullSource() || !IsWindow(cfg_.source.hwnd) || capture.Closed()) {
                LatchHotkeys(hotkeys.get());
                return !stop_.load();
            }
            const std::string why = WindowStateHint(cfg_.source.hwnd);
            if (!why.empty() && why != wait.sourceHint) {
                wait.sourceHint = why;
                Publish(wait);
            }
            if (hotkeys)
                hotkeys->PumpUntil(QpcNow() + freq / 10);
            else
                Sleep(100);
        }
    } else {
        const int64_t firstFrameDeadline = QpcNow() + freq * 3;
        while (!grabFrame()) {
            if (stop_.load() || QpcNow() > firstFrameDeadline)
                throw std::runtime_error("Не пришло ни одного кадра за 3 секунды");
            Sleep(5);
        }
    }
    if (audio)
        audio->Start();

    EngineStatus st;
    st.running = true;
    st.ready = true;
    st.source = std::format("{}x{}", setup.video.width, setup.video.height);
    st.videoWidth = setup.video.width;
    st.videoHeight = setup.video.height;
    const char* codecName = cfg_.codec == Codec::H264 ? "H.264" : "HEVC";
    st.encoderName = encoder.Name();
    st.gpu = d3d.adapterName;
    st.encoder = std::format("{} {} {}x{}@{} {} кбит/с", st.encoderName, codecName, setup.video.width,
                             setup.video.height, cfg_.fps, cfg_.bitrateKbps);
    st.audio = setup.audio.has_value();
    st.replayEnabled = replay.has_value();
    st.hotkeysFailed = hotkeys ? hotkeys->Failed() : 0;
    Publish(st);
    LogInfo("Кодировщик: %s (%s)", st.encoderName.c_str(), st.gpu.c_str());

    std::unique_ptr<OverlayHud> hud;
    if (cfg_.overlay)
        hud = std::make_unique<OverlayHud>(HudMonitor(cfg_.source));
    const std::string hudEncoder = st.encoderName + " " + codecName;

    ScreenshotReader screenshots;
    BackgroundJobs jobs;
    std::unique_ptr<RecordingSession> rec;
    bool startRecord = !cfg_.initialRecordPath.empty();
    std::wstring nextRecordPath = cfg_.initialRecordPath;
    std::optional<int64_t> replayCutoff;
    int64_t replayDueQpc = 0;
    bool encoding = false;
    bool paused = false;
    int64_t pauseSkip = 0;     // сколько pts паузы уже вычтено из таймера
    int64_t pauseHoldPts = -1; // pts кадра, на котором таймер замер
    std::vector<PacketPtr> audioBatch;

    const int64_t frameDuration = kTimescale / cfg_.fps;
    const int64_t frameQpc = freq / cfg_.fps;
    const int64_t maxRecord = cfg_.maxRecordSeconds > 0 ? std::llround(cfg_.maxRecordSeconds * kTimescale) : 0;
    // Сколько ждать хвост аудио после остановки: больше задержки микшера (200 мс) с запасом.
    const int64_t audioTailQpc = freq * 3 / 2;
    const int64_t replayDelayQpc = freq / 2;

    const auto route = [&](const PacketPtr& p) {
        if (replay)
            replay->Push(p);
        if (rec)
            rec->OnPacket(*p);
    };
    // Вызывается в потоке кодирования; запись и replay живут в этом потоке, поэтому через очередь.
    PacketQueue videoQueue;
    const auto onVideo = [&videoQueue, frameDuration](const EncodedPacket& e) {
        auto p = std::make_shared<Packet>();
        p->stream = StreamKind::Video;
        p->pts = e.pts;
        p->duration = frameDuration;
        p->keyframe = e.keyframe;
        p->data.assign(e.data.begin(), e.data.end());
        videoQueue.Push(std::move(p));
    };
    std::vector<PacketPtr> videoBatch;
    const auto routeVideo = [&] {
        videoQueue.TakeAll(videoBatch);
        for (const auto& p : videoBatch)
            route(p);
        videoBatch.clear();
    };

    // Полсекунды очереди: дольше кодировщик не успевает — кадры честно пропускаются.
    EncodeQueue encodeQueue(d3d, encoder, setup.video.width, setup.video.height,
                            std::max<size_t>(8, cfg_.fps / 2), onVideo);
    std::vector<int64_t> slotPts;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    int64_t pts = 0;
    std::unique_ptr<LiveStream> live;
    std::vector<int64_t> streamPts;
    const auto startStream = [&](int64_t tickQpc) {
        StreamConfig sc;
        {
            std::lock_guard lock(streamMtx_);
            sc = streamCfg_;
        }
        if (sc.url.empty() || sc.key.empty()) {
            st.streamError = "укажите сервер и ключ потока";
            LogWarn("Трансляция: %s", st.streamError.c_str());
            return;
        }
        try {
            auto ls = std::make_unique<LiveStream>();
            ls->cfg = sc;
            VideoStreamInfo& v = ls->info;
            v.codec = sc.codec;
            v.height = std::min(std::max(sc.height, 144u), setup.video.height) & ~1u;
            v.width = static_cast<uint32_t>(std::llround(static_cast<double>(setup.video.width) * v.height /
                                                         setup.video.height)) & ~1u;
            v.fps = std::clamp(sc.fps, 1u, cfg_.fps);
            v.bitrateKbps = std::clamp(sc.bitrateKbps, 300u, 50000u);
            v.keyintSeconds = 2;  // требование большинства площадок
            v.lowLatency = true;
            ls->encoder = CreateVideoEncoder(d3d, v, cfg_.encoder);

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = v.width;
            desc.Height = v.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ThrowIfFailed(d3d.device->CreateTexture2D(&desc, nullptr, ls->tex.put()), "CreateTexture2D(stream)");
            ls->scaler = std::make_unique<Scaler>(d3d.device.get(), stage.get(), ls->tex.get());
            try {
                ls->overlays = std::make_unique<OverlayCompositor>(d3d.device.get(), ls->tex.get());
            } catch (const std::exception& e) {
                LogWarn("Виджеты эфира недоступны: %s", e.what());
            }

            ls->startQpc = tickQpc;
            ls->basePts = clock.PtsFromQpc(tickQpc);
            ls->publisher = std::make_shared<RtmpPublisher>(StreamTarget{ sc.url, sc.key, sc.reconnect }, v,
                                                            ls->encoder->SequenceHeader(), setup.audio, ls->basePts);
            RtmpPublisher* pub = ls->publisher.get();
            ls->queue = std::make_unique<EncodeQueue>(d3d, *ls->encoder, v.width, v.height,
                                                      std::max<size_t>(4, v.fps / 2),
                                                      [pub](const EncodedPacket& e) { pub->PushVideo(e); });
            st.streamInfo = std::format("{} {} {}x{}@{} {} кбит/с", ls->encoder->Name(),
                                        v.codec == Codec::H264 ? "H.264" : "HEVC", v.width, v.height, v.fps,
                                        v.bitrateKbps);
            st.streamError.clear();
            LogInfo("Трансляция: второй кодировщик %s", st.streamInfo.c_str());
            if (sc.alsoRecord && !rec && !startRecord) {
                startRecord = true;
                nextRecordPath = MakeOutputPath(cfg_.outDir, L"live", L"mp4");
                ls->startedRecording = true;
            }
            live = std::move(ls);
        } catch (const std::exception& e) {
            st.streamError = e.what();
            LogError("Трансляция не запущена: %s", e.what());
        } catch (const winrt::hresult_error& e) {
            st.streamError = std::format("0x{:08X}", static_cast<unsigned>(e.code()));
            LogError("Трансляция не запущена: %s", st.streamError.c_str());
        }
    };
    // Закрытие сети (вежливый FCUnpublish, до 1,5 с) — в фоне, чтобы не сбить темп кадров.
    const auto stopStream = [&](int64_t tickQpc) {
        if (!live)
            return;
        live->queue.reset();
        if (live->startedRecording && rec && !rec->Stopping())
            rec->RequestStop(pts, tickQpc + audioTailQpc);
        jobs.Run([pub = std::move(live->publisher)]() mutable { pub.reset(); });
        live.reset();
        LogInfo("■ Трансляция остановлена");
    };

    TimerPeriod timerPeriod;
    const int64_t start = QpcNow();
    bool quit = false;
    bool retarget = false;
    int64_t fpsWindowStart = start;
    int fpsFrames = 0;

    for (int64_t frame = 0; !quit; ++frame) {
        int64_t deadline = start + MulDiv64(frame, freq, cfg_.fps);
        if (cfg_.hotkeys) {
            bool dirty = false;
            HotkeyBinds binds;
            {
                std::lock_guard lock(hotkeyMtx_);
                if (hotkeysDirty_) {
                    hotkeysDirty_ = false;
                    binds = hotkeyBinds_;
                    dirty = true;
                }
            }
            if (dirty) {
                hotkeys = std::make_unique<Hotkeys>(binds, true, replay.has_value(), true, true, true);
                st.hotkeysFailed = hotkeys->Failed();
            }
        }
        if (hotkeys)
            hotkeys->PumpUntil(deadline);
        else
            while (QpcNow() < deadline)
                Sleep(1);

        int64_t now = QpcNow();
        // Постоянный FPS: каждая пропущенная ячейка сетки получает текущий кадр (дубль), чтобы в файле
        // не было дыр по времени и плеер не дёргался. Дубли ставятся в очередь кодирования пачкой.
        slotPts.clear();
        slotPts.push_back(clock.PtsFromQpc(deadline));
        if (now - deadline > frameQpc) {
            const int64_t late = (now - deadline) / frameQpc;
            for (int64_t k = 1; k <= late; ++k)
                slotPts.push_back(clock.PtsFromQpc(start + MulDiv64(frame + k, freq, cfg_.fps)));
            frame += late;
            deadline = start + MulDiv64(frame, freq, cfg_.fps);
        }
        pts = slotPts.back();
        if (audio)
            audio->SetGains(systemGain_.load(), micGain_.load());
        if (encodeQueue.Failed())
            throw std::runtime_error(encodeQueue.Error());
        // Смену источника смотрим до команд: клик REC во время переключения не теряется.
        const bool windowNow = cfg_.source.type == SourceSpec::Type::Window;
        const bool sourceGone = windowNow && (!IsWindow(cfg_.source.hwnd) || capture.Closed());
        const bool switching = PullSource() || sourceGone;
        if (switching) {
            LatchHotkeys(hotkeys.get());
            retarget = !stop_.load();
            quit = true;
        } else {
            quit = stop_.load() || capture.Closed();
            st.sourceHint = windowNow ? WindowStateHint(cfg_.source.hwnd) : std::string{};
        }

        bool wantToggle = false;
        bool wantReplay = false;
        bool wantShot = false;
        bool wantStream = false;
        bool wantPause = false;
        if (!switching) {
            wantToggle = cmdToggle_.exchange(false);
            wantReplay = cmdReplay_.exchange(false);
            wantShot = cmdShot_.exchange(false);
            wantStream = cmdStream_.exchange(false);
            wantPause = cmdPause_.exchange(false);
            if (hotkeys) {
                wantToggle |= hotkeys->Take(HotkeyAction::ToggleRecord);
                wantReplay |= hotkeys->Take(HotkeyAction::SaveReplay);
                wantShot |= hotkeys->Take(HotkeyAction::Screenshot);
                wantStream |= hotkeys->Take(HotkeyAction::ToggleStream);
                wantPause |= hotkeys->Take(HotkeyAction::TogglePause);
            }
        }

        if (!quit && wantStream) {
            if (live)
                stopStream(now);
            else
                startStream(deadline);
        }
        if (!quit && wantPause && (paused || (rec && !rec->Stopping()) || live)) {
            if (!paused) {
                paused = true;
                if (rec && !rec->Stopping())
                    pauseHoldPts = pts;
            } else {
                if (pauseHoldPts >= 0) {
                    pauseSkip += pts - pauseHoldPts;
                    pauseHoldPts = -1;
                }
                paused = false;
            }
        }

        if (!quit) {
            if (wantToggle) {
                if (rec && !rec->Stopping())
                    rec->RequestStop(pts, now + audioTailQpc);
                else if (!rec && !startRecord) {
                    startRecord = true;
                    nextRecordPath = MakeOutputPath(cfg_.outDir, L"rec", L"mp4");
                }
            }
            if (wantReplay) {
                if (!replay)
                    LogWarn("Replay выключен — задайте длительность буфера");
                else if (!replayCutoff) {
                    replayCutoff = pts;
                    replayDueQpc = now + replayDelayQpc;
                }
            }
            if (wantShot) {
                try {
                    auto img = std::make_shared<CpuImage>(
                        screenshots.Read(d3d.device.get(), d3d.context.get(), stage.get()));
                    const auto path = MakeOutputPath(cfg_.outDir, L"shot", L"png");
                    jobs.Run([img, path] {
                        SavePng(*img, path);
                        LogInfo("▣ Скриншот: %s", WideToUtf8(path).c_str());
                    });
                } catch (const std::exception& e) {
                    LogError("Скриншот: %s", e.what());
                }
            }
        }

        if (rec && !rec->Stopping() && maxRecord > 0 && pts - rec->StartPts() >= maxRecord)
            rec->RequestStop(pts, now + audioTailQpc);

        bool forceIdr = false;
        if (startRecord && !rec && !quit) {
            startRecord = false;
            try {
                rec = std::make_unique<RecordingSession>(nextRecordPath, setup, pts);
                forceIdr = true;
                pauseSkip = 0;
                pauseHoldPts = paused ? pts : -1;
            } catch (const std::exception& e) {
                if (cfg_.exitAfterRecording)
                    throw;
                LogError("Не удалось начать запись: %s", e.what());
            }
        }

        const bool needEncode = !quit && (replay.has_value() || (rec && !rec->Stopping()));
        // Пауза держит уже собранный кадр: новый захват не берём, кодировщик повторяет его.
        if (!paused)
            grabFrame();
        if (needEncode) {
            // Запись началась на этом тике — дубли пропущенных ячеек до её начала не нужны.
            const size_t first = forceIdr ? slotPts.size() - 1 : 0;
            const size_t want = slotPts.size() - first;
            const size_t queued = encodeQueue.Submit(stage.get(), slotPts.data() + first, want, forceIdr || !encoding);
            if (queued > 0) {
                st.duplicatedFrames += queued - 1;
                ++fpsFrames;
            }
            st.droppedFrames += want - queued;
            encoding = queued > 0 || encoding;
        } else {
            encoding = false;
        }

        // Трансляция: своя сетка CFR (обычно 30 fps), тот же кадр stage, масштаб на GPU.
        if (live && live->queue->Failed()) {
            st.streamError = live->queue->Error();
            LogError("Трансляция: %s", st.streamError.c_str());
            stopStream(now);
        }
        if (live && !quit) {
            streamPts.clear();
            for (;;) {
                const int64_t due = live->startQpc + MulDiv64(live->frame, freq, live->info.fps);
                if (due > deadline)
                    break;
                streamPts.push_back(clock.PtsFromQpc(due));
                ++live->frame;
            }
            // Долгий провал (> 0,5 с) не догоняем пачкой дублей — в эфире это всё равно не видно.
            const size_t maxBurst = std::max<size_t>(1, live->info.fps / 2);
            if (streamPts.size() > maxBurst)
                streamPts.erase(streamPts.begin(), streamPts.end() - static_cast<ptrdiff_t>(maxBurst));
            if (!streamPts.empty()) {
                // Пока пауза — в эфир уходит уже нарисованный кадр, без нового захвата и без виджетов поверх.
                if (!paused || !live->haveFrame) {
                    live->scaler->Draw(d3d.context.get());
                    if (live->overlays) {
                        std::vector<OverlaySprite> sprites;
                        {
                            std::lock_guard lock(overlayMtx_);
                            sprites = overlays_;
                        }
                        live->overlays->Draw(d3d.context.get(), sprites);
                    }
                    live->haveFrame = true;
                }
                const bool idr = live->needIdr || live->publisher->TakeKeyframeRequest();
                const size_t q = live->queue->Submit(live->tex.get(), streamPts.data(), streamPts.size(), idr);
                live->needIdr = idr && q == 0;
            }
        }
        routeVideo();

        audioQueue.TakeAll(audioBatch);
        if (!paused) {
            for (const auto& p : audioBatch) {
                route(p);
                if (live)
                    live->publisher->PushAudio(p);
            }
        }
        audioBatch.clear();

        if (rec && rec->ReadyToFinish(QpcNow())) {
            rec->Finish();
            rec.reset();
            if (cfg_.exitAfterRecording)
                quit = true;
        }

        if (replayCutoff && QpcNow() >= replayDueQpc) {
            auto packets = replay->Snapshot(*replayCutoff);
            replayCutoff.reset();
            if (packets.empty()) {
                LogWarn("Replay-буфер пока пуст");
            } else {
                LogInfo("Сохраняю повтор (буфер %.1f МБ)…", static_cast<double>(replay->Bytes()) / (1024.0 * 1024.0));
                const auto path = MakeOutputPath(cfg_.outDir, L"replay", L"mp4");
                jobs.Run([packets = std::move(packets), setup, path] { WriteClip(path, setup, packets); });
            }
        }

        now = QpcNow();
        if (now - fpsWindowStart >= freq) {
            st.fps = static_cast<double>(fpsFrames) * static_cast<double>(freq) / static_cast<double>(now - fpsWindowStart);
            fpsFrames = 0;
            fpsWindowStart = now;
        }
        if (paused && !(rec && !rec->Stopping()) && !live) {
            if (pauseHoldPts >= 0) {
                pauseSkip += pts - pauseHoldPts;
                pauseHoldPts = -1;
            }
            paused = false;
        }
        st.recording = rec != nullptr;
        st.paused = paused;
        st.stopping = rec && rec->Stopping();
        // На паузе кадры в файл всё ещё идут (чтобы не рвать время), а счётчик на экране стоит.
        if (!rec) {
            st.recordSeconds = 0.0;
        } else {
            int64_t elapsed = pts - rec->StartPts() - pauseSkip;
            if (pauseHoldPts >= 0)
                elapsed -= pts - pauseHoldPts;
            if (elapsed < 0)
                elapsed = 0;
            st.recordSeconds = static_cast<double>(elapsed) / kTimescale;
        }
        st.recordPath = rec ? rec->Path() : std::wstring();
        st.replayMB = replay ? static_cast<double>(replay->Bytes()) / (1024.0 * 1024.0) : 0.0;
        if (live) {
            st.streamState = live->publisher->State();
            st.streamSeconds = static_cast<double>(pts - live->basePts) / kTimescale;
            st.streamKbps = live->publisher->Kbps();
            const std::string err = live->publisher->Error();
            if (!err.empty())
                st.streamError = err;
            if (st.streamState == StreamState::Live)
                st.streamError.clear();
            if (st.streamState == StreamState::Failed)
                stopStream(now);
        } else if (st.streamState != StreamState::Failed) {
            st.streamState = StreamState::Off;
            st.streamSeconds = 0.0;
            st.streamKbps = 0.0;
        }
        Publish(st);

        if (hud) {
            HudState hs;
            hs.visible = st.recording;
            hs.stopping = st.stopping;
            hs.paused = st.paused;
            hs.seconds = st.recordSeconds;
            hs.fps = st.fps;
            hs.encoder = hudEncoder;
            hud->Update(hs);
        }
    }
    hud.reset();
    stopStream(QpcNow());
    encodeQueue.Close();
    routeVideo();

    // Аудиопоток дожимает AAC до текущего момента, остаток уходит в запись.
    if (audio)
        audio->Stop();
    audioQueue.TakeAll(audioBatch);
    if (rec) {
        rec->RequestStop(pts, 0);
        for (const auto& p : audioBatch)
            rec->OnPacket(*p);
        rec->Finish();
        rec.reset();
    }

    encoder.Flush();
    capture.Stop();
    if (capture.Closed() && !retarget)
        LogWarn("Источник захвата закрыт");
    return retarget && !stop_.load();
}
