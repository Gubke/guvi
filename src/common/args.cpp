#include "common/args.h"
#include "common/hr.h"

#include <cstdio>
#include <cwchar>
#include <stdexcept>

std::string WideToUtf8(std::wstring_view s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void PrintUsage()
{
    std::printf(
        "Использование:\n"
        "  recorder.exe (--monitor N | --window \"заголовок\") [опции] [out.mp4]\n\n"
        "Видео:\n"
        "  --seconds S         длительность записи в out.mp4 (0 — до F9/Ctrl+C, по умолчанию 0)\n"
        "  --fps N             частота кадров (60)\n"
        "  --bitrate K         битрейт видео, кбит/с (20000)\n"
        "  --codec C           h264 | hevc (h264)\n"
        "  --keyint S          интервал IDR, секунд (2)\n"
        "  --encoder E         auto | nvenc | amf | qsv (auto: по видеокарте)\n"
        "  --height H          уменьшить до высоты H с сохранением пропорций (0 — как у источника)\n"
        "  --no-cursor         не рисовать курсор\n"
        "  --no-overlay        не показывать HUD «ЗАПИСЬ» (в видео он и так не попадает)\n"
        "Аудио:\n"
        "  --no-audio          без звука\n"
        "  --no-system-audio   без системного звука (loopback)\n"
        "  --mic               добавить микрофон по умолчанию\n"
        "  --mic-gain G        усиление микрофона (1.0)\n"
        "  --system-gain G     громкость системного звука (1.0)\n"
        "  --audio-bitrate K   AAC: 96 | 128 | 160 | 192 кбит/с (192)\n"
        "  --speaker \"имя\"     устройство воспроизведения для loopback (часть имени)\n"
        "  --mic-device \"имя\"  микрофон (часть имени)\n"
        "  --list-devices      показать аудиоустройства и камеры и выйти\n"
        "Веб-камера поверх видео:\n"
        "  --webcam [\"имя\"]    включить (часть имени; без имени — первая камера)\n"
        "  --webcam-size P     ширина, %% кадра (25)\n"
        "  --webcam-x X        0 — слева … 100 — справа (100)\n"
        "  --webcam-y Y        0 — сверху … 100 — снизу (100)\n"
        "Replay и горячие клавиши:\n"
        "  --replay N          держать последние N секунд в RAM, F8 — сохранить (0 — выкл.)\n"
        "  --out-dir DIR       папка для записей по F9, повторов и скриншотов (.)\n"
        "  --no-hotkeys        не регистрировать F8/F9/F10\n\n"
        "Без out.mp4 или с --replay программа работает до Ctrl+C: F9 — старт/стоп записи,\n"
        "F8 — сохранить повтор, F10 — скриншот PNG.\n"
        "Выход: .mp4 (видео+AAC) или .h264/.264/.hevc/.h265/.265 (сырой Annex B, без звука).\n");
}

namespace {

long long ParseInt(const wchar_t* s, const char* name, long long lo, long long hi)
{
    wchar_t* end = nullptr;
    const long long v = std::wcstoll(s, &end, 10);
    if (end == s || *end != L'\0' || v < lo || v > hi)
        throw std::invalid_argument(std::format("Неверное значение {}", name));
    return v;
}

double ParseDouble(const wchar_t* s, const char* name, double lo, double hi)
{
    wchar_t* end = nullptr;
    const double v = std::wcstod(s, &end);
    if (end == s || *end != L'\0' || !(v >= lo && v <= hi))
        throw std::invalid_argument(std::format("Неверное значение {}", name));
    return v;
}

} // namespace

Args ParseArgs(int argc, wchar_t** argv)
{
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        auto next = [&](const char* name) -> const wchar_t* {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::format("После {} нужно значение", name));
            return argv[++i];
        };

        if (arg == L"--monitor")
            a.monitor = static_cast<int>(ParseInt(next("--monitor"), "--monitor", 0, 63));
        else if (arg == L"--window")
            a.windowTitle = next("--window");
        else if (arg == L"--seconds")
            a.seconds = ParseDouble(next("--seconds"), "--seconds", 0.0, 24.0 * 3600.0);
        else if (arg == L"--fps")
            a.fps = static_cast<uint32_t>(ParseInt(next("--fps"), "--fps", 1, 240));
        else if (arg == L"--bitrate")
            a.bitrateKbps = static_cast<uint32_t>(ParseInt(next("--bitrate"), "--bitrate", 100, 500'000));
        else if (arg == L"--keyint")
            a.keyintSeconds = static_cast<uint32_t>(ParseInt(next("--keyint"), "--keyint", 1, 10));
        else if (arg == L"--codec") {
            const std::wstring_view c = next("--codec");
            if (c == L"h264")
                a.codec = Codec::H264;
            else if (c == L"hevc" || c == L"h265")
                a.codec = Codec::HEVC;
            else
                throw std::invalid_argument("--codec: h264 или hevc");
        }
        else if (arg == L"--encoder") {
            const std::wstring_view e = next("--encoder");
            if (e != L"auto" && e != L"nvenc" && e != L"amf" && e != L"qsv")
                throw std::invalid_argument("--encoder: auto, nvenc, amf или qsv");
            a.encoder = e;
        }
        else if (arg == L"--no-cursor")
            a.cursor = false;
        else if (arg == L"--no-overlay")
            a.overlay = false;
        else if (arg == L"--height")
            a.outputHeight = static_cast<uint32_t>(ParseInt(next("--height"), "--height", 0, 4320));
        else if (arg == L"--system-gain")
            a.systemGain = static_cast<float>(ParseDouble(next("--system-gain"), "--system-gain", 0.0, 4.0));
        else if (arg == L"--speaker")
            a.speakerDevice = next("--speaker");
        else if (arg == L"--mic-device") {
            a.micDevice = next("--mic-device");
            a.microphone = true;
        }
        else if (arg == L"--list-devices")
            a.listDevices = true;
        else if (arg == L"--webcam") {
            a.webcam = true;
            // Имя камеры необязательно: следующий аргумент, если это не другой ключ и не out.mp4.
            if (i + 1 < argc && std::wstring_view(argv[i + 1]).rfind(L"--", 0) != 0 &&
                !std::wstring_view(argv[i + 1]).ends_with(L".mp4"))
                a.webcamDevice = argv[++i];
        }
        else if (arg == L"--webcam-size")
            a.webcamLayout.size = static_cast<float>(ParseDouble(next("--webcam-size"), "--webcam-size", 5.0, 100.0));
        else if (arg == L"--webcam-x")
            a.webcamLayout.x = static_cast<float>(ParseDouble(next("--webcam-x"), "--webcam-x", 0.0, 100.0));
        else if (arg == L"--webcam-y")
            a.webcamLayout.y = static_cast<float>(ParseDouble(next("--webcam-y"), "--webcam-y", 0.0, 100.0));
        else if (arg == L"--no-audio")
            a.systemAudio = a.microphone = false;
        else if (arg == L"--no-system-audio")
            a.systemAudio = false;
        else if (arg == L"--mic")
            a.microphone = true;
        else if (arg == L"--mic-gain")
            a.micGain = static_cast<float>(ParseDouble(next("--mic-gain"), "--mic-gain", 0.0, 8.0));
        else if (arg == L"--audio-bitrate") {
            const auto k = static_cast<uint32_t>(ParseInt(next("--audio-bitrate"), "--audio-bitrate", 96, 192));
            if (k != 96 && k != 128 && k != 160 && k != 192)
                throw std::invalid_argument("--audio-bitrate: 96, 128, 160 или 192");
            a.audioBitrateKbps = k;
        }
        else if (arg == L"--replay")
            a.replaySeconds = static_cast<uint32_t>(ParseInt(next("--replay"), "--replay", 0, 600));
        else if (arg == L"--out-dir")
            a.outDir = next("--out-dir");
        else if (arg == L"--no-hotkeys")
            a.hotkeys = false;
        else if (arg == L"-h" || arg == L"--help")
            throw std::invalid_argument("");
        else if (!arg.empty() && arg[0] == L'-')
            throw std::invalid_argument(std::format("Неизвестный параметр {}", WideToUtf8(arg)));
        else if (a.output.empty())
            a.output = arg;
        else
            throw std::invalid_argument("Указано больше одного выходного файла");
    }

    if (a.listDevices)
        return a;
    if (a.monitor.has_value() == !a.windowTitle.empty())
        throw std::invalid_argument("Нужно ровно одно из: --monitor N или --window \"заголовок\"");
    if (a.output.empty() && a.replaySeconds == 0 && !a.hotkeys)
        throw std::invalid_argument("Нечего делать: нет out.mp4, нет --replay и выключены горячие клавиши");
    if (a.replaySeconds > 0 && !a.hotkeys)
        std::printf("Внимание: --replay без горячих клавиш — повтор сохранить будет нечем\n");
    return a;
}
