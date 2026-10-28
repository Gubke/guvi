#pragma once

#include "audio/aac_encoder.h"
#include "audio/wasapi_source.h"
#include "common/clock.h"
#include "common/types.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

struct AudioOptions {
    bool systemAudio = true;
    bool microphone = false;
    float systemGain = 1.0f;
    float micGain = 1.0f;
    uint32_t bitrateKbps = 192;
    std::wstring renderDeviceId;   // пусто — по умолчанию
    std::wstring captureDeviceId;  // пусто — по умолчанию
};

// Модуль 3: system loopback + микрофон → микшер на общей шкале MediaClock → AAC.
// Каждый источник раскладывается по абсолютной позиции сэмпла (из QPC-времени пакета),
// поэтому дыры (loopback молчит, когда ничего не играет) заполняются тишиной, а звук
// остаётся синхронным с видео, у которого PTS считается от того же t0.
class AudioEngine {
public:
    using PacketFn = std::function<void(PacketPtr)>;

    AudioEngine(const MediaClock& clock, const AudioOptions& opt, PacketFn out);
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    const AudioStreamInfo& Info() const { return encoder_->Info(); }

    void Start();
    // Останавливает поток, дожимает AAC-кодер; все пакеты отданы в out до возврата.
    void Stop();

    // Громкость и mute (0) без перезапуска: читаются микшером на каждом блоке.
    void SetGains(float system, float mic)
    {
        systemGain_ = system;
        micGain_ = mic;
    }

private:
    // Дорожка на общей шкале времени. Часы устройства уплывают относительно QPC на десятки ppm;
    // вместо скачков (вставка тишины / пропуск) поток слегка растягивается или сжимается
    // линейным ресемплером (до ±0,5 %), пока ошибка положения не уйдёт в ноль.
    struct Track {
        std::unique_ptr<WasapiSource> source;
        bool mic = false;  // громкость берётся из systemGain_ / micGain_ (меняется на лету)
        int64_t start = 0;          // абсолютная позиция (в кадрах) первого кадра buf
        std::vector<float> buf;     // interleaved стерео, уже на шкале микшера

        std::vector<float> hist;    // вход ресемплера, ещё не выданный в buf
        double phase = 0.0;         // позиция следующего выходного кадра внутри hist
        double errEma = 0.0;        // сглаженная ошибка положения, кадры (+ — опережаем QPC)
        double step = 1.0;          // входных кадров на один выходной
        bool primed = false;

        int64_t End() const { return start + static_cast<int64_t>(buf.size() / WasapiSource::kChannels); }
        void Write(int64_t pos, bool hasPos, const float* data, uint32_t frames);
        void Resample(const float* data, uint32_t frames);
        void ResetResampler();
        void MixInto(int64_t pos, uint32_t frames, float* out, float gain) const;
        void DiscardBefore(int64_t pos);
    };

    void ThreadMain();
    void PollSources();
    void MixReady(bool flushAll);
    int64_t FramePosFromPts(int64_t pts) const;

    MediaClock clock_;
    PacketFn out_;
    std::vector<Track> tracks_;
    std::unique_ptr<AacEncoder> encoder_;
    int64_t mixPos_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<float> systemGain_{ 1.0f };
    std::atomic<float> micGain_{ 1.0f };
};
