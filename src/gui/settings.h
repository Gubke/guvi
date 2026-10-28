#pragma once

#include <windows.h>

#include <string>

// Виджет ссылки на эфире: те же проценты, что у веб-камеры (размер и свободное место).
struct StreamWidget {
    bool enabled = true;
    std::string url;
    float size = 28.0f;
    float x = 0.0f;
    float y = 0.0f;
    bool operator==(const StreamWidget&) const = default;
};

inline constexpr int kMaxStreamWidgets = 4;

struct GuiSettings {
    bool sourceIsWindow = false;
    int monitor = 0;
    std::wstring windowTitle;  // окно ищется по точному заголовку при следующем запуске

    int codec = 0;    // 0 — H.264, 1 — HEVC
    int encoder = 0;  // 0 — авто, 1 — NVENC, 2 — AMD AMF, 3 — Intel Quick Sync
    int outputHeight = 0;  // 0 — как у источника
    int fps = 60;
    int bitrateKbps = 20000;
    int keyintSeconds = 2;
    bool cursor = true;

    bool systemAudio = true;
    bool microphone = false;
    float systemGain = 1.0f;
    float micGain = 1.0f;
    int audioBitrateKbps = 192;
    std::wstring renderDeviceId;   // пусто — по умолчанию
    std::wstring captureDeviceId;  // пусто — по умолчанию

    int replaySeconds = 30;
    std::wstring outDir;
    bool overlay = true;
    int librarySort = 0;  // 0 — новые, 1 — старые, 2 — имя, 3 — размер

    bool webcam = false;
    std::wstring webcamDeviceId;  // пусто — первая камера
    float webcamSize = 25.0f;     // % ширины кадра
    float webcamX = 100.0f;       // % свободного места по горизонтали (0 — слева)
    float webcamY = 100.0f;       // % по вертикали (0 — сверху)
    int webcamMode = 0;           // 0 — всегда записывать

    int preset = 0;               // 0 — низкая задержка, 1 — качество
    bool systemMuted = false;     // mute — громкость 0 на лету, дорожка остаётся
    bool micMuted = false;
    int hkRecordVk = VK_F9, hkRecordMods = 0;
    int hkReplayVk = VK_F8, hkReplayMods = 0;
    int hkShotVk = VK_F10, hkShotMods = 0;
    int hkStreamVk = VK_F9, hkStreamMods = MOD_CONTROL;
    int hkPauseVk = VK_XBUTTON1, hkPauseMods = 0;  // Mouse4

    // Трансляция. Ключ хранится в JSON только зашифрованным DPAPI (текущий пользователь Windows).
    std::string streamUrl;
    std::string streamKey;
    int streamCodec = 0;  // 0 — H.264, 1 — HEVC (Enhanced RTMP)
    int streamHeight = 720;
    int streamFps = 30;
    int streamBitrateKbps = 2500;
    bool streamReconnect = true;
    bool streamAlsoRecord = false;

    int widgetCount = 0;
    StreamWidget widgets[kMaxStreamWidgets]{};

    std::string language;  // пусто — как в Windows: ru, uk, en, pl

    bool operator==(const GuiSettings&) const = default;
};

// recorder_gui.json рядом с exe.
std::wstring SettingsPath();
GuiSettings LoadSettings();
bool SaveSettings(const GuiSettings& s);
