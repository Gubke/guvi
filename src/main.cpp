#include "app/recorder_engine.h"
#include "audio/wasapi_source.h"
#include "capture/webcam_source.h"
#include "common/args.h"
#include "common/hr.h"

#include <unknwn.h>
#include <winrt/base.h>

#include <atomic>
#include <cstdio>

namespace {

std::atomic<RecorderEngine*> g_engine{ nullptr };

BOOL WINAPI OnConsoleCtrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (auto* e = g_engine.load())
            e->RequestStop();
        return TRUE;
    }
    return FALSE;
}

// Пусто, если part пустой; бросает, если устройство с такой частью имени не найдено.
std::wstring ResolveDevice(const std::wstring& part, bool capture)
{
    if (part.empty())
        return {};
    const std::string needle = WideToUtf8(part);
    for (const auto& d : ListAudioDevices(capture))
        if (d.name.find(needle) != std::string::npos)
            return d.id;
    throw std::runtime_error(std::format("{} «{}» не найден (см. --list-devices)",
                                         capture ? "Микрофон" : "Устройство воспроизведения", needle));
}

void PrintDevices()
{
    for (const bool capture : { false, true }) {
        std::printf("%s:\n", capture ? "Микрофоны (--mic-device)" : "Воспроизведение / loopback (--speaker)");
        for (const auto& d : ListAudioDevices(capture))
            std::printf("  %s%s\n", d.name.c_str(), d.isDefault ? "  [по умолчанию]" : "");
    }
    std::printf("Камеры (--webcam):\n");
    for (const auto& d : ListVideoDevices())
        std::printf("  %s%s\n", d.name.c_str(), d.directShow ? "  [DirectShow]" : "");
}

// Пусто — первая камера; бросает, если камеры с такой частью имени нет.
std::wstring ResolveCamera(const std::wstring& part)
{
    if (part.empty())
        return {};
    const std::string needle = WideToUtf8(part);
    for (const auto& d : ListVideoDevices())
        if (d.name.find(needle) != std::string::npos)
            return d.id;
    throw std::runtime_error(std::format("Камера «{}» не найдена (см. --list-devices)", needle));
}

EncoderPref ParseEncoder(const std::wstring& e)
{
    if (e == L"nvenc")
        return EncoderPref::Nvenc;
    if (e == L"amf")
        return EncoderPref::Amf;
    if (e == L"qsv")
        return EncoderPref::Qsv;
    return EncoderPref::Auto;
}

EngineConfig ToEngineConfig(const Args& a)
{
    EngineConfig c;
    if (a.monitor) {
        c.source.type = SourceSpec::Type::Monitor;
        c.source.monitor = *a.monitor;
    } else {
        c.source.type = SourceSpec::Type::WindowTitle;
        c.source.title = a.windowTitle;
    }
    c.codec = a.codec;
    c.encoder = ParseEncoder(a.encoder);
    c.renderDeviceId = ResolveDevice(a.speakerDevice, false);
    c.captureDeviceId = ResolveDevice(a.micDevice, true);
    c.overlay = a.overlay;
    c.outputHeight = a.outputHeight;
    c.systemGain = a.systemGain;
    c.fps = a.fps;
    c.bitrateKbps = a.bitrateKbps;
    c.keyintSeconds = a.keyintSeconds;
    c.cursor = a.cursor;
    c.systemAudio = a.systemAudio;
    c.microphone = a.microphone;
    c.micGain = a.micGain;
    c.audioBitrateKbps = a.audioBitrateKbps;
    c.replaySeconds = a.replaySeconds;
    c.outDir = a.outDir;
    c.hotkeys = a.hotkeys;
    c.initialRecordPath = a.output;
    c.maxRecordSeconds = a.seconds;
    c.exitAfterRecording = !a.Interactive();
    return c;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    Args args;
    try {
        args = ParseArgs(argc, argv);
    } catch (const std::invalid_argument& e) {
        if (*e.what())
            std::fprintf(stderr, "Ошибка: %s\n\n", e.what());
        PrintUsage();
        return 2;
    }

    // COM для перечисления аудиоустройств; поток движка инициализирует свой апартамент сам.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (args.listDevices) {
        PrintDevices();
        return 0;
    }

    try {
        RecorderEngine engine(ToEngineConfig(args));
        std::shared_ptr<WebcamSource> webcam;
        if (args.webcam) {
            webcam = std::make_shared<WebcamSource>(ResolveCamera(args.webcamDevice));
            engine.SetWebcam(webcam);
            engine.SetWebcamLayout(args.webcamLayout);
        }
        g_engine = &engine;
        if (args.Interactive())
            std::printf("Работаю. Ctrl+C — выход.\n");
        engine.Run();
        g_engine = nullptr;
        return 0;
    } catch (const winrt::hresult_error& e) {
        std::fprintf(stderr, "Ошибка WinRT 0x%08X: %s\n", static_cast<unsigned>(e.code()),
                     WideToUtf8(e.message()).c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Ошибка: %s\n", e.what());
    }
    g_engine = nullptr;
    return 1;
}
