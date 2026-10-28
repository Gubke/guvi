#pragma once

#include "capture/webcam_layout.h"
#include "common/types.h"

#include <optional>
#include <string>

struct Args {
    std::optional<int> monitor;     // --monitor N
    std::wstring windowTitle;       // --window "часть заголовка"
    double seconds = 0.0;           // 0 — без ограничения
    uint32_t fps = 60;
    uint32_t bitrateKbps = 20000;
    uint32_t keyintSeconds = 2;
    Codec codec = Codec::H264;
    std::wstring encoder = L"auto"; // auto | nvenc | amf | qsv
    bool cursor = true;

    bool systemAudio = true;
    bool microphone = false;
    float micGain = 1.0f;
    float systemGain = 1.0f;
    uint32_t outputHeight = 0;      // 0 — как у источника
    uint32_t audioBitrateKbps = 192;
    std::wstring speakerDevice;     // часть имени устройства воспроизведения (loopback)
    std::wstring micDevice;         // часть имени микрофона
    bool listDevices = false;       // --list-devices: вывести устройства и выйти

    bool webcam = false;            // --webcam [имя]
    std::wstring webcamDevice;      // часть имени камеры; пусто — первая
    WebcamLayout webcamLayout;

    uint32_t replaySeconds = 0;     // 0 — replay-буфер выключен
    bool hotkeys = true;
    bool overlay = true;
    std::wstring outDir = L".";

    std::wstring output;            // пусто — запись только по F9 в outDir

    // Интерактивный режим: работаем до Ctrl+C, а не до конца первой записи.
    bool Interactive() const { return replaySeconds > 0 || output.empty(); }
};

// Бросает std::invalid_argument с текстом ошибки (пустой текст — показать справку).
Args ParseArgs(int argc, wchar_t** argv);

void PrintUsage();
