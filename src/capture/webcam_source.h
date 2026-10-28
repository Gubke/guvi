#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IMFMediaSource;

struct VideoDevice {
    std::wstring id;   // символьная ссылка Media Foundation или "dshow:<путь/имя>"
    std::string name;  // UTF-8
    bool directShow = false;
};

// Камеры Media Foundation + камеры, которые есть только в DirectShow (виртуальные: NVIDIA Broadcast, OBS и т.п.).
// Вызывать из потока с инициализированным COM.
std::vector<VideoDevice> ListVideoDevices();

struct WebcamFrame {
    std::vector<uint8_t> bgra;  // width*height*4, сверху вниз, без отступов
    uint32_t width = 0;
    uint32_t height = 0;
    bool alpha = false;  // устройство отдаёт настоящую прозрачность
    uint64_t seq = 0;    // растёт с каждым кадром
};

// Захват камеры в своём потоке; последний кадр в памяти (BGRA). Потокобезопасна.
// Один объект делят GUI (превью) и движок (композитинг), поэтому камера не переоткрывается
// при перезапуске движка.
class WebcamSource {
public:
    explicit WebcamSource(std::wstring deviceId);  // пустой id — первая камера
    ~WebcamSource();

    WebcamSource(const WebcamSource&) = delete;
    WebcamSource& operator=(const WebcamSource&) = delete;

    // Копирует последний кадр, если он новее knownSeq.
    bool CopyIfNewer(uint64_t knownSeq, WebcamFrame& out) const;

    const std::wstring& DeviceId() const { return deviceId_; }
    std::string Status() const;  // "NVIDIA Broadcast 1280×720" / ошибка / "запуск…"
    bool Failed() const { return failed_; }

private:
    void Thread();
    void RunMediaFoundation(const std::wstring& link);
    void RunDirectShow(const std::wstring& key);
    void SetStatus(std::string s, bool failed);

public:
    // Из потока захвата (MF или колбэк DirectShow). pitch < 0 — снизу вверх, data указывает на верхнюю строку.
    void Publish(const uint8_t* data, long pitch, uint32_t w, uint32_t h, bool alpha);

private:

    std::wstring deviceId_;
    std::atomic<bool> quit_{ false };
    std::atomic<bool> failed_{ false };
    HANDLE wake_ = nullptr;  // будит поток DirectShow при остановке

    mutable std::mutex mtx_;
    WebcamFrame frame_;
    std::string status_ = "запуск…";
    IMFMediaSource* shutdownTarget_ = nullptr;  // чтобы прервать блокирующий ReadSample из деструктора

    std::vector<uint8_t> scratch_;  // только поток захвата
    std::thread thread_;
};
