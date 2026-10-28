#pragma once

#include "app/hotkeys.h"
#include "capture/overlay_compositor.h"
#include "capture/webcam_layout.h"
#include "common/types.h"
#include "encode/video_encoder.h"
#include "stream/rtmp_publisher.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class WebcamSource;

struct SourceSpec {
    enum class Type { Monitor, Window, WindowTitle };
    Type type = Type::Monitor;
    int monitor = 0;
    HWND hwnd = nullptr;
    std::wstring title;  // для WindowTitle — поиск по подстроке
};

// Трансляция: второй кодировщик из тех же кадров, уменьшенный. Меняется без перезапуска движка.
struct StreamConfig {
    std::string url;  // rtmp:// или rtmps://
    std::string key;  // не логируется
    Codec codec = Codec::H264;
    uint32_t height = 720;
    uint32_t fps = 30;
    uint32_t bitrateKbps = 2500;
    bool reconnect = true;
    bool alsoRecord = false;  // на время трансляции писать и локальный файл
};

struct EngineConfig {
    SourceSpec source;
    Codec codec = Codec::H264;
    EncoderPref encoder = EncoderPref::Auto;
    uint32_t outputHeight = 0;  // 0 — как у источника; иначе уменьшить до этой высоты (с пропорциями)
    uint32_t fps = 60;
    uint32_t bitrateKbps = 20000;
    uint32_t keyintSeconds = 2;
    bool lowLatency = true;  // пресет кодировщика: низкая задержка / качество
    bool cursor = true;

    bool systemAudio = true;
    bool microphone = false;
    float systemGain = 1.0f;
    float micGain = 1.0f;
    uint32_t audioBitrateKbps = 192;
    std::wstring renderDeviceId;   // loopback; пусто — по умолчанию
    std::wstring captureDeviceId;  // микрофон; пусто — по умолчанию

    uint32_t replaySeconds = 0;
    std::wstring outDir = L".";
    bool hotkeys = true;
    HotkeyBinds hotkeyBinds;
    bool overlay = true;  // HUD «ЗАПИСЬ» поверх экрана, исключённый из захвата

    std::wstring initialRecordPath;  // начать запись сразу (CLI)
    double maxRecordSeconds = 0.0;   // 0 — без ограничения
    bool exitAfterRecording = false; // CLI без --replay
};

struct EngineStatus {
    bool running = false;   // поток движка жив
    bool ready = false;     // захват и кодер инициализированы
    bool recording = false;
    bool paused = false;    // последний кадр удерживается, новое аудио в файл не пишется
    bool stopping = false;  // ждём хвост аудио
    double recordSeconds = 0.0;
    std::wstring recordPath;

    std::string source;
    uint32_t videoWidth = 0;  // размер кадра в файле (после масштаба)
    uint32_t videoHeight = 0;
    std::string encoder;      // полная строка: "NVENC H.264 1920x1080@60 20000 кбит/с"
    std::string encoderName;  // бэкенд: "NVENC" / "AMD AMF" / "Intel Quick Sync"
    std::string gpu;          // имя адаптера
    bool audio = false;
    double fps = 0.0;
    uint64_t droppedFrames = 0;     // ячейки CFR, которые не попали в очередь (кодировщик не успевает)
    uint64_t duplicatedFrames = 0;  // ячейки, заполненные повтором кадра (цикл захвата опоздал)
    uint32_t hotkeysFailed = 0;     // биты 1 << HotkeyAction: сочетание занято другой программой

    bool replayEnabled = false;
    double replayMB = 0.0;

    StreamState streamState = StreamState::Off;
    double streamSeconds = 0.0;
    double streamKbps = 0.0;
    std::string streamInfo;   // "NVENC H.264 1280x720@30 2500 кбит/с"
    std::string streamError;  // без ключа

    std::string error;
    std::string sourceHint;  // окно свёрнуто / закрыто / ещё не видно — не ошибка
};

// Цикл записи: захват → аппаратный кодировщик (NVENC / AMF / Quick Sync), аудио, запись по F9/команде, replay-буфер, скриншоты.
// Используется и CLI (Run в главном потоке), и GUI (StartThread).
class RecorderEngine {
public:
    explicit RecorderEngine(EngineConfig cfg);
    ~RecorderEngine();

    RecorderEngine(const RecorderEngine&) = delete;
    RecorderEngine& operator=(const RecorderEngine&) = delete;

    // Блокирует до RequestStop() / закрытия источника / конца записи (exitAfterRecording).
    // Ошибки инициализации бросаются наружу.
    void Run();

    // Run() в отдельном потоке; ошибка попадает в Status().error.
    void StartThread();
    void RequestStop();
    void Join();

    void ToggleRecord() { cmdToggle_ = true; }
    void SaveReplay() { cmdReplay_ = true; }
    void Screenshot() { cmdShot_ = true; }
    void ToggleStream() { cmdStream_ = true; }
    void TogglePause() { cmdPause_ = true; }
    // Берётся при каждом старте трансляции (кнопка LIVE или горячая клавиша).
    void SetStreamConfig(const StreamConfig& c)
    {
        std::lock_guard lock(streamMtx_);
        streamCfg_ = c;
    }

    // Веб-камера поверх видео: источник (nullptr — выкл.) и положение. Меняются на лету, без перезапуска.
    void SetWebcam(std::shared_ptr<WebcamSource> source);
    void SetWebcamLayout(const WebcamLayout& layout);
    // Горячие клавиши без перезапуска захвата. Виджеты — только в кадр эфира.
    void SetHotkeyBinds(const HotkeyBinds& binds);
    void SetStreamOverlays(std::vector<OverlaySprite> overlays);
    // Смена монитора или окна без Join на потоке UI: подхватит цикл захвата.
    void SetSource(const SourceSpec& source);
    // Громкость дорожек (0 — mute) на лету; сами дорожки включаются только при запуске.
    void SetAudioGains(float system, float mic)
    {
        systemGain_ = system;
        micGain_ = mic;
    }

    EngineStatus Status() const;
    const EngineConfig& Config() const { return cfg_; }

private:
    void Publish(const EngineStatus& s);
    // true — источник сменился или окно пропало, сессию надо собрать заново.
    bool RunOnce();
    // Ждёт, пока окно можно захватывать. false — движок останавливают.
    bool WaitForWindow(const std::string& why);
    bool PullSource();
    void LatchHotkeys(Hotkeys* keys);

    EngineConfig cfg_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> cmdToggle_{ false };
    std::atomic<bool> cmdReplay_{ false };
    std::atomic<bool> cmdShot_{ false };
    std::atomic<bool> cmdStream_{ false };
    std::atomic<bool> cmdPause_{ false };
    std::mutex streamMtx_;
    StreamConfig streamCfg_;

    std::mutex hotkeyMtx_;
    HotkeyBinds hotkeyBinds_;
    bool hotkeysDirty_ = false;

    std::mutex overlayMtx_;
    std::vector<OverlaySprite> overlays_;

    std::mutex sourceMtx_;
    SourceSpec pendingSource_;
    bool hasPendingSource_ = false;

    std::atomic<float> systemGain_{ 1.0f };
    std::atomic<float> micGain_{ 1.0f };

    std::mutex webcamMtx_;
    std::shared_ptr<WebcamSource> webcam_;
    WebcamLayout webcamLayout_;

    mutable std::mutex statusMtx_;
    EngineStatus status_;
    std::thread thread_;
};
