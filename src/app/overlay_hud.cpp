#include "app/overlay_hud.h"
#include "common/i18n.h"
#include "common/log.h"

#include <algorithm>
#include <cmath>

#include <objidl.h>
// Заголовкам GDI+ нужны min/max, а проект собирается с NOMINMAX.
namespace Gdiplus {
using std::max;
using std::min;
}
#include <gdiplus.h>

#include <format>
#include <string>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

constexpr wchar_t kClassName[] = L"ScreenRecorderHud";
constexpr int kMargin = 16;

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring Elapsed(double seconds)
{
    const auto t = static_cast<int>(std::max(0.0, seconds));
    return std::format(L"{:02}:{:02}:{:02}", t / 3600, (t / 60) % 60, t % 60);
}

void AddRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r)
{
    const float d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

} // namespace

OverlayHud::OverlayHud(HMONITOR monitor)
    : monitor_(monitor)
{
    thread_ = std::thread(&OverlayHud::ThreadMain, this);
}

OverlayHud::~OverlayHud()
{
    quit_ = true;
    if (const DWORD id = threadId_.load())
        PostThreadMessageW(id, WM_NULL, 0, 0);
    if (thread_.joinable())
        thread_.join();
}

void OverlayHud::Update(const HudState& s)
{
    std::lock_guard lock(mtx_);
    state_ = s;
}

void OverlayHud::ThreadMain()
{
    threadId_ = GetCurrentThreadId();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr) != Gdiplus::Ok) {
        LogWarn("HUD: GDI+ не запустился");
        return;
    }

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // повторная регистрация из другого движка просто вернёт ошибку

    // WS_EX_TRANSPARENT + LAYERED — клики проходят насквозь; NOACTIVATE — не крадёт фокус у игры.
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kClassName, L"REC", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    bool excluded = false;
    if (hwnd) {
        excluded = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) != FALSE;
        if (!excluded)
            LogWarn("HUD выключен: Windows не умеет исключать окно из захвата (нужна 10 2004+)");
    }

    bool shown = false;
    std::wstring lastKey;
    while (!quit_ && hwnd && excluded) {
        HudState s;
        {
            std::lock_guard lock(mtx_);
            s = state_;
        }
        const bool blink = (GetTickCount64() / 500) % 2 == 0;
        if (s.visible) {
            // Перерисовываем, только когда что-то видимое изменилось.
            const std::wstring key = std::format(L"{}|{}|{}|{:.0f}|{}|{}|{}", s.stopping, s.paused,
                                                 static_cast<int>(s.seconds), s.fps, Utf8ToWide(s.encoder), blink,
                                                 Utf8ToWide(I18nLanguage()));
            if (key != lastKey || !shown) {
                Render(hwnd, s, blink);
                lastKey = key;
            }
            if (!shown) {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                shown = true;
            }
            // Полноэкранные окна могут подняться выше — периодически возвращаем HUD наверх.
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (shown) {
            ShowWindow(hwnd, SW_HIDE);
            shown = false;
        }

        MsgWaitForMultipleObjects(0, nullptr, FALSE, 120, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }

    if (hwnd)
        DestroyWindow(hwnd);
    Gdiplus::GdiplusShutdown(gdiToken);
}

void OverlayHud::Render(HWND hwnd, const HudState& s, bool blink)
{
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR mon = monitor_ ? monitor_ : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi))
        return;
    const UINT dpi = GetDpiForWindow(hwnd);
    const float scale = static_cast<float>(dpi ? dpi : 96) / 96.0f;
    const int margin = static_cast<int>(kMargin * scale);

    const std::wstring line1 = s.stopping ? TrW(L"СОХРАНЕНИЕ…")
                             : s.paused   ? TrW(L"ПАУЗА") + L"  " + Elapsed(s.seconds)
                                          : TrW(L"ЗАПИСЬ") + L"  " + Elapsed(s.seconds);
    const std::wstring line2 = std::format(L"{:.0f} fps  ·  {}", s.fps, Utf8ToWide(s.encoder));
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font big(&family, 14.5f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font minor(&family, 12.0f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    const float pad = 10 * scale;
    const float dot = 11 * scale;
    const float textX = pad + dot + 8 * scale;
    float line1H = 16 * scale;
    float line2H = 14 * scale;
    int w = 1;
    int h = 1;
    {
        HDC screen = GetDC(nullptr);
        Gdiplus::Graphics measure(screen);
        measure.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
        const Gdiplus::RectF layout(0, 0, 4000, 400);
        Gdiplus::RectF b1, b2;
        measure.MeasureString(line1.c_str(), -1, &big, layout, &format, &b1);
        measure.MeasureString(line2.c_str(), -1, &minor, layout, &format, &b2);
        ReleaseDC(nullptr, screen);
        line1H = std::max(b1.Height, 1.0f);
        line2H = std::max(b2.Height, 1.0f);
        const float w1 = textX + b1.Width + pad;
        const float w2 = pad + b2.Width + pad;
        w = std::max(1, static_cast<int>(std::ceil(std::max(w1, w2))));
        h = std::max(1, static_cast<int>(std::ceil(pad + line1H + 2 * scale + line2H + pad * 0.35f)));
    }
    POINT pos{ mi.rcWork.right - w - margin, mi.rcWork.top + margin };

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    std::fill_n(static_cast<uint32_t*>(bits), static_cast<size_t>(w) * h, 0u);

    {
        // Рисуем прямо в память DIB с premultiplied alpha — её и ждёт UpdateLayeredWindow.
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        Gdiplus::GraphicsPath bg;
        AddRoundRect(bg, 0.5f, 0.5f, static_cast<float>(w) - 1, static_cast<float>(h) - 1, 10 * scale);
        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(215, 18, 18, 22));
        g.FillPath(&bgBrush, &bg);
        Gdiplus::Pen border(Gdiplus::Color(200, 227, 38, 54), 1.5f * scale);
        g.DrawPath(&border, &bg);

        Gdiplus::SolidBrush red(s.paused ? Gdiplus::Color(255, 255, 190, 70)
                                          : Gdiplus::Color(s.stopping || blink ? 255 : 90, 227, 38, 54));
        g.FillEllipse(&red, pad, pad + 3 * scale, dot, dot);

        Gdiplus::SolidBrush white(Gdiplus::Color(255, 240, 240, 244));
        Gdiplus::SolidBrush muted(Gdiplus::Color(255, 160, 162, 170));

        g.DrawString(line1.c_str(), -1, &big, Gdiplus::PointF(textX, pad - 2 * scale), &white);
        g.DrawString(line2.c_str(), -1, &minor, Gdiplus::PointF(pad, pad + line1H + 2 * scale), &muted);
    }

    SIZE size{ w, h };
    POINT src{ 0, 0 };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, screen, &pos, &size, mem, &src, 0, &blend, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}
