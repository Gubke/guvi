#pragma once

#include <unknwn.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <winrt/base.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AudioDevice {
    std::wstring id;   // IMMDevice::GetId; пусто — устройство по умолчанию
    std::string name;  // UTF-8
    bool isDefault = false;
};

// Активные устройства: capture=false — воспроизведение (для loopback), true — микрофоны.
std::vector<AudioDevice> ListAudioDevices(bool capture);

// Модуль 3: один WASAPI-источник в shared mode. Формат всегда float32 / 48 кГц / стерео:
// AUTOCONVERTPCM ставит в аудиодвижке ресемплер и матрицу каналов (микрофон моно → стерео).
class WasapiSource {
public:
    enum class Kind { SystemLoopback, Microphone };

    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;

    // frames кадров по kChannels float; data == nullptr — тишина (AUDCLNT_BUFFERFLAGS_SILENT).
    // qpcHns — QPC-время первого кадра в 100 нс, либо kNoTimestamp (AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR).
    static constexpr int64_t kNoTimestamp = INT64_MIN;
    using PacketFn = std::function<void(int64_t qpcHns, const float* data, uint32_t frames)>;

    // deviceId пустой — устройство по умолчанию; если указанного нет — тоже по умолчанию.
    WasapiSource(Kind kind, const std::wstring& deviceId);
    ~WasapiSource();

    WasapiSource(const WasapiSource&) = delete;
    WasapiSource& operator=(const WasapiSource&) = delete;

    void Start();
    void Stop();
    void Drain(const PacketFn& fn);

    const std::string& Name() const { return name_; }
    Kind GetKind() const { return kind_; }

private:
    Kind kind_;
    std::string name_;
    winrt::com_ptr<IAudioClient> client_;
    winrt::com_ptr<IAudioCaptureClient> capture_;
    bool started_ = false;
};
