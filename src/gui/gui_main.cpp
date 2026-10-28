#include "app/recorder_engine.h"
#include "app/recording_session.h"
#include "audio/wasapi_source.h"
#include "capture/webcam_layout.h"
#include "capture/webcam_source.h"
#include "capture/wgc_capture.h"
#include "common/hr.h"
#include "common/log.h"
#include "common/i18n.h"
#include "common/version.h"
#include "gui/library.h"
#include "gui/settings.h"
#include "gui/url_image.h"

#include <unknwn.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <winhttp.h>
#include <winrt/base.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// ---------- D3D11 для отрисовки UI ----------
// Отдельное устройство: у движка своё (на адаптере NVIDIA) и оно пересоздаётся при каждой смене
// настроек, а окно UI должно жить всё время.
struct UiD3D {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<IDXGISwapChain> swapChain;
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    UINT resizeWidth = 0;
    UINT resizeHeight = 0;
};

UiD3D g_d3d;

// Назначение горячей клавиши: пока target >= 0, следующее нажатие перехватывается окном.
struct KeyCapture {
    int target = -1;
    bool done = false;
    bool cancel = false;
    bool clear = false;
    UINT vk = 0;
    UINT mods = 0;
};
KeyCapture g_keyCapture;

bool HandleKeyCapture(UINT msg, WPARAM wParam)
{
    if (g_keyCapture.target < 0 || g_keyCapture.done)
        return false;
    UINT vk = 0;
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
        vk = static_cast<UINT>(wParam);
    else if (msg == WM_XBUTTONDOWN)
        vk = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    else if (msg == WM_MBUTTONDOWN)
        vk = VK_MBUTTON;
    else if (msg == WM_RBUTTONDOWN)
        vk = VK_RBUTTON;
    else
        return false;
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL:
    case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
        return true;  // ждём основную клавишу
    case VK_ESCAPE:
        g_keyCapture.cancel = true;
        break;
    case VK_BACK:
        g_keyCapture.clear = true;
        break;
    default:
        g_keyCapture.vk = vk;
        g_keyCapture.mods = ((GetKeyState(VK_CONTROL) & 0x8000) ? MOD_CONTROL : 0) |
                            ((GetKeyState(VK_MENU) & 0x8000) ? MOD_ALT : 0) |
                            ((GetKeyState(VK_SHIFT) & 0x8000) ? MOD_SHIFT : 0);
        break;
    }
    g_keyCapture.done = true;
    return true;
}

void CreateRenderTarget()
{
    winrt::com_ptr<ID3D11Texture2D> back;
    g_d3d.swapChain->GetBuffer(0, IID_PPV_ARGS(back.put()));
    g_d3d.device->CreateRenderTargetView(back.get(), nullptr, g_d3d.rtv.put());
}

bool CreateDeviceD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, g_d3d.swapChain.put(), g_d3d.device.put(),
                                               nullptr, g_d3d.context.put());
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, g_d3d.swapChain.put(), g_d3d.device.put(), nullptr,
                                           g_d3d.context.put());
    if (FAILED(hr))
        return false;
    CreateRenderTarget();
    return true;
}

// Своя тёмная шапка. Прямоугольники — в координатах клиента (как у ImGui без multi-viewport).
// WM_NCHITTEST приходит в экранных: сравнивать их напрямую нельзя, после сдвига окна
// шапка перестаёт попадать в прямоугольник и клик уходит в HTCLIENT.
struct CaptionBar {
    RECT gear{};
    RECT minimize{};
    RECT maximize{};
    RECT close{};
    RECT bar{};
    bool ready = false;
};
CaptionBar g_caption;

bool CaptionClientPoint(HWND hWnd, LPARAM lParam, bool fromScreen, POINT& client)
{
    client = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (!fromScreen)
        return true;
    return ScreenToClient(hWnd, &client) != FALSE;
}

bool CaptionDragPoint(const POINT& client)
{
    if (!g_caption.ready || !PtInRect(&g_caption.bar, client))
        return false;
    if (PtInRect(&g_caption.gear, client) || PtInRect(&g_caption.minimize, client) ||
        PtInRect(&g_caption.maximize, client) || PtInRect(&g_caption.close, client))
        return false;
    return true;
}

void BeginCaptionDrag(HWND hWnd)
{
    if (GetCapture() == hWnd)
        ReleaseCapture();
    SendMessageW(hWnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
}

LRESULT CaptionHitTest(HWND hWnd, LPARAM lParam)
{
    POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    RECT rc{};
    GetWindowRect(hWnd, &rc);
    if (!IsZoomed(hWnd)) {
        const int border = std::max(6, MulDiv(8, static_cast<int>(GetDpiForWindow(hWnd)), 96));
        const bool left = pt.x < rc.left + border;
        const bool right = pt.x >= rc.right - border;
        const bool top = pt.y < rc.top + border;
        const bool bottom = pt.y >= rc.bottom - border;
        if (top && left)
            return HTTOPLEFT;
        if (top && right)
            return HTTOPRIGHT;
        if (bottom && left)
            return HTBOTTOMLEFT;
        if (bottom && right)
            return HTBOTTOMRIGHT;
        if (left)
            return HTLEFT;
        if (right)
            return HTRIGHT;
        if (top)
            return HTTOP;
        if (bottom)
            return HTBOTTOM;
    }
    POINT client{};
    if (!CaptionClientPoint(hWnd, lParam, true, client) || !CaptionDragPoint(client))
        return HTCLIENT;
    return HTCAPTION;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (HandleKeyCapture(msg, wParam))
        return msg == WM_XBUTTONDOWN ? TRUE : 0;
    if (msg == WM_NCHITTEST)
        return CaptionHitTest(hWnd, lParam);
    // Перетаскивание шапки запускаем сами: ImGui держит SetCapture, и после отпускания
    // кнопки следующий клик приходит как клиентский — DefWindowProc его уже не везёт.
    if (msg == WM_NCLBUTTONDOWN && wParam == HTCAPTION) {
        BeginCaptionDrag(hWnd);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        POINT client{};
        if (CaptionClientPoint(hWnd, lParam, false, client) && CaptionDragPoint(client)) {
            BeginCaptionDrag(hWnd);
            return 0;
        }
    }
    if (msg == WM_NCCALCSIZE && wParam) {
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        if (IsZoomed(hWnd)) {
            HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(mon, &mi))
                params->rgrc[0] = mi.rcWork;
        }
        return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_d3d.resizeWidth = LOWORD(lParam);
            g_d3d.resizeHeight = HIWORD(lParam);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(hWnd);
        mmi->ptMinTrackSize.x = MulDiv(900, static_cast<int>(dpi), 96);
        mmi->ptMinTrackSize.y = MulDiv(620, static_cast<int>(dpi), 96);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------- Тема ----------
const ImVec4 kAccent{ 0.89f, 0.15f, 0.21f, 1.0f };
const ImVec4 kAccentHover{ 0.97f, 0.27f, 0.31f, 1.0f };
const ImVec4 kAccentActive{ 0.72f, 0.09f, 0.14f, 1.0f };
const ImVec4 kMuted{ 0.55f, 0.56f, 0.60f, 1.0f };
const ImVec4 kOk{ 0.40f, 0.85f, 0.55f, 1.0f };
const ImVec4 kWarn{ 1.00f, 0.75f, 0.30f, 1.0f };
const ImVec4 kPane{ 0.10f, 0.10f, 0.12f, 1.0f };
const ImVec4 kTile{ 0.14f, 0.14f, 0.165f, 1.0f };
const ImVec4 kTileHover{ 0.19f, 0.19f, 0.22f, 1.0f };

void ApplyTheme(float dpi)
{
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = { 14, 12 };
    s.FramePadding = { 10, 6 };
    s.ItemSpacing = { 10, 8 };
    s.ItemInnerSpacing = { 8, 6 };
    s.ScrollbarSize = 12;
    s.GrabMinSize = 12;
    s.WindowRounding = 0;
    s.ChildRounding = 10;
    s.FrameRounding = 6;
    s.PopupRounding = 6;
    s.GrabRounding = 6;
    s.ScrollbarRounding = 6;
    s.TabRounding = 6;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameBorderSize = 0;
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = { 0, 4 };
    s.ScaleAllSizes(dpi);
    s.FontScaleDpi = dpi;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = { 0.93f, 0.93f, 0.95f, 1.0f };
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = { 0.065f, 0.065f, 0.078f, 1.0f };
    c[ImGuiCol_ChildBg] = kPane;
    c[ImGuiCol_PopupBg] = { 0.11f, 0.11f, 0.13f, 0.98f };
    c[ImGuiCol_Border] = { 0.20f, 0.20f, 0.23f, 1.0f };
    c[ImGuiCol_FrameBg] = { 0.15f, 0.15f, 0.175f, 1.0f };
    c[ImGuiCol_FrameBgHovered] = { 0.20f, 0.20f, 0.23f, 1.0f };
    c[ImGuiCol_FrameBgActive] = { 0.24f, 0.15f, 0.17f, 1.0f };
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_Button] = { 0.18f, 0.18f, 0.21f, 1.0f };
    c[ImGuiCol_ButtonHovered] = { 0.25f, 0.25f, 0.29f, 1.0f };
    c[ImGuiCol_ButtonActive] = { 0.30f, 0.17f, 0.19f, 1.0f };
    c[ImGuiCol_Header] = { 0.89f, 0.15f, 0.21f, 0.35f };
    c[ImGuiCol_HeaderHovered] = { 0.89f, 0.15f, 0.21f, 0.55f };
    c[ImGuiCol_HeaderActive] = { 0.89f, 0.15f, 0.21f, 0.75f };
    c[ImGuiCol_Separator] = { 0.22f, 0.22f, 0.25f, 1.0f };
    c[ImGuiCol_ScrollbarBg] = { 0, 0, 0, 0 };
    c[ImGuiCol_ScrollbarGrab] = { 0.22f, 0.22f, 0.25f, 1.0f };
    c[ImGuiCol_ScrollbarGrabHovered] = { 0.30f, 0.30f, 0.34f, 1.0f };
    c[ImGuiCol_ScrollbarGrabActive] = kAccent;
    c[ImGuiCol_TextSelectedBg] = { 0.89f, 0.15f, 0.21f, 0.35f };
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_Tab] = { 0.12f, 0.12f, 0.14f, 1.0f };
    c[ImGuiCol_TabHovered] = { 0.45f, 0.14f, 0.17f, 1.0f };
    c[ImGuiCol_TabSelected] = { 0.55f, 0.12f, 0.16f, 1.0f };
    c[ImGuiCol_TabDimmed] = { 0.12f, 0.12f, 0.14f, 1.0f };
    c[ImGuiCol_TabDimmedSelected] = { 0.28f, 0.12f, 0.14f, 1.0f };
    c[ImGuiCol_TitleBg] = { 0.10f, 0.10f, 0.12f, 1.0f };
    c[ImGuiCol_TitleBgActive] = { 0.16f, 0.09f, 0.10f, 1.0f };
    c[ImGuiCol_TitleBgCollapsed] = { 0.10f, 0.10f, 0.12f, 1.0f };
    c[ImGuiCol_ModalWindowDimBg] = { 0, 0, 0, 0.55f };
}

ImU32 Col(const ImVec4& v, float alphaMul = 1.0f)
{
    return ImGui::GetColorU32(ImVec4{ v.x, v.y, v.z, v.w * alphaMul });
}

bool AccentButton(const char* label, const ImVec2& size = { 0, 0 })
{
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1, 1, 1, 1 });
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

std::string FormatElapsed(double seconds)
{
    const auto t = static_cast<int>(std::max(0.0, seconds));
    return std::format("{:02}:{:02}:{:02}", t / 3600, (t / 60) % 60, t % 60);
}

std::string FormatBytes(uint64_t b)
{
    const double v = static_cast<double>(b);
    if (v >= 1024.0 * 1024 * 1024)
        return std::format("{:.1f} ГБ", v / (1024.0 * 1024 * 1024));
    if (v >= 1024.0 * 1024)
        return std::format("{:.1f} МБ", v / (1024.0 * 1024));
    return std::format("{:.0f} КБ", v / 1024.0);
}

// Обрезка UTF-8 строки по ширине с «…» (режем только по границе символа).
std::string Ellipsize(const std::string& text, float maxWidth)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
        return text;
    std::string s = text;
    while (!s.empty()) {
        size_t cut = s.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut;
        s.resize(cut);
        const std::string candidate = s + "…";
        if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth)
            return candidate;
    }
    return "…";
}

std::wstring PickFolder(HWND owner, const std::wstring& current)
{
    winrt::com_ptr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put()))))
        return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Папка для записей");
    winrt::com_ptr<IShellItem> start;
    if (!current.empty() && SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr, IID_PPV_ARGS(start.put()))))
        dlg->SetFolder(start.get());
    if (FAILED(dlg->Show(owner)))
        return {};
    winrt::com_ptr<IShellItem> item;
    if (FAILED(dlg->GetResult(item.put())))
        return {};
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        out = path;
    CoTaskMemFree(path);
    return out;
}

void PlayFile(HWND owner, const std::wstring& path)
{
    ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ShowInFolder(HWND owner, const std::wstring& path)
{
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(owner, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// Квадратная кнопка с нарисованным динамиком; при mute — красный крест. true — переключили.
bool MuteButton(const char* id, bool& muted)
{
    const float s = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, { s, s });
    if (pressed)
        muted = !muted;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 bg = muted ? ImVec4{ 0.35f, 0.10f, 0.13f, 1.0f } : style.Colors[hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button];
    dl->AddRectFilled(p, { p.x + s, p.y + s }, Col(bg), style.FrameRounding);
    const ImU32 fg = muted ? Col(kAccentHover) : Col(ImVec4{ 0.85f, 0.86f, 0.90f, 1.0f });
    const float u = s / 16.0f;
    const ImVec2 o{ p.x + 3 * u, p.y };
    const ImVec2 body[] = { { o.x, p.y + 6 * u }, { o.x + 3 * u, p.y + 6 * u }, { o.x + 7 * u, p.y + 3 * u },
                            { o.x + 7 * u, p.y + 13 * u }, { o.x + 3 * u, p.y + 10 * u }, { o.x, p.y + 10 * u } };
    dl->AddConvexPolyFilled(body, 6, fg);
    if (muted) {
        dl->AddLine({ o.x + 9 * u, p.y + 6 * u }, { o.x + 12 * u, p.y + 10 * u }, fg, 1.6f * u);
        dl->AddLine({ o.x + 12 * u, p.y + 6 * u }, { o.x + 9 * u, p.y + 10 * u }, fg, 1.6f * u);
    } else {
        dl->PathArcTo({ o.x + 7 * u, p.y + 8 * u }, 3 * u, -0.9f, 0.9f, 8);
        dl->PathStroke(fg, 1.3f * u);
        dl->PathArcTo({ o.x + 7 * u, p.y + 8 * u }, 5.5f * u, -0.9f, 0.9f, 10);
        dl->PathStroke(fg, 1.3f * u);
    }
    return pressed;
}

// Подсказка, которая показывается и над выключенным элементом.
void HoverTip(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", text);
}

constexpr int kFpsPresets[] = { 24, 30, 48, 60, 90, 120, 144 };
constexpr int kAacRates[] = { 96, 128, 160, 192 };
constexpr int kHeights[] = { 0, 2160, 1440, 1080, 720, 480 };
constexpr const char* kHeightNames[] = { "Как у источника", "2160p (4K)", "1440p", "1080p", "720p", "480p" };
constexpr const char* kSortNames[] = { "Сначала новые", "Сначала старые", "По имени", "По размеру" };

// ---------- Приложение ----------
constexpr const char* kTips[] = {
    "Запиши гадину во всей красе",
    "Всегда на охране твоего покоя",
    "Каждый админ упадёт к твоим ногам",
    "Снимай так, будто это их последний патч",
    "Пусть чат завидует твоему битрейту",
    "Сегодня ты режиссёр, они — массовка",
    "Ни один баг не спрячется от кадра",
    "Запись включена, отмазки выключены",
    "Красиво положишь ролик — красиво лягут споры",
    "Твой клип, их оправдания",
    "Камера не врёт, в отличие от патчноутов",
    "Сними это, пока не передумали краснеть",
    "Тише едешь — хуже клип. Пиши громко",
    "Здесь рождаются легенды и чужие извинения",
    "GuVi смотрит. GuVi помнит. GuVi пишет.",
};

class App {
public:
    explicit App(HWND hwnd) : hwnd_(hwnd)
    {
        s_ = LoadSettings();
        saved_ = s_;
        tip_ = kTips[static_cast<size_t>(GetTickCount64() % std::size(kTips))];
        strncpy_s(urlBuf_, s_.streamUrl.c_str(), _TRUNCATE);
        strncpy_s(keyBuf_, s_.streamKey.c_str(), _TRUNCATE);
        for (int i = 0; i < kMaxStreamWidgets; ++i)
            strncpy_s(widgetUrl_[i], s_.widgets[i].url.c_str(), _TRUNCATE);
        restored_ = RecoverUnfinishedRecordings(s_.outDir);
        RefreshSources();
        if (s_.sourceIsWindow) {
            for (size_t i = 0; i < windows_.size(); ++i)
                if (windows_[i].title == s_.windowTitle)
                    windowSel_ = static_cast<int>(i);
        }
        StartEngine();
        I18nUse(s_.language);
    }

    ~App()
    {
        if (updateThread_.joinable())
            updateThread_.join();
        StopEngine();
        SaveSettings(s_);
        SecureZeroMemory(keyBuf_, sizeof(keyBuf_));
    }

    void Draw(ImFont* bold);

private:
    void RefreshSources()
    {
        monitors_ = ListMonitors();
        HWND selected = nullptr;
        std::wstring title = s_.windowTitle;
        if (windowSel_ >= 0 && windowSel_ < static_cast<int>(windows_.size())) {
            selected = windows_[static_cast<size_t>(windowSel_)].hwnd;
            title = windows_[static_cast<size_t>(windowSel_)].title;
        }
        windows_ = ListWindows(hwnd_);
        windowSel_ = -1;
        if (selected) {
            for (size_t i = 0; i < windows_.size(); ++i)
                if (windows_[i].hwnd == selected)
                    windowSel_ = static_cast<int>(i);
        }
        // Закрытое окно ищем по заголовку, когда оно откроется снова.
        if (windowSel_ < 0 && !title.empty()) {
            for (size_t i = 0; i < windows_.size(); ++i)
                if (windows_[i].title == title) {
                    windowSel_ = static_cast<int>(i);
                    break;
                }
        }
        s_.monitor = std::clamp(s_.monitor, 0, std::max(0, static_cast<int>(monitors_.size()) - 1));
        renderDevices_ = ListAudioDevices(false);
        captureDevices_ = ListAudioDevices(true);
        cameras_ = ListVideoDevices();
    }

    WebcamLayout Layout() const { return { s_.webcamSize, s_.webcamX, s_.webcamY }; }

    // Камера живёт в GUI (общая для превью и движка), поэтому не переоткрывается при «Применить».
    void SyncWebcam()
    {
        const bool want = s_.webcam;
        if (webcam_ && (!want || webcam_->DeviceId() != s_.webcamDeviceId)) {
            if (engine_)
                engine_->SetWebcam(nullptr);
            webcam_.reset();
            previewSeq_ = 0;
        }
        if (want && !webcam_)
            webcam_ = std::make_shared<WebcamSource>(s_.webcamDeviceId);
        if (engine_) {
            engine_->SetWebcam(webcam_);
            engine_->SetWebcamLayout(Layout());
        }
    }

    // Последний кадр камеры → текстура UI-устройства для превью.
    void UpdatePreview()
    {
        if (!webcam_ || !webcam_->CopyIfNewer(previewSeq_, previewFrame_))
            return;
        previewSeq_ = previewFrame_.seq;
        if (!previewSrv_ || previewW_ != previewFrame_.width || previewH_ != previewFrame_.height) {
            previewTex_ = nullptr;
            previewSrv_ = nullptr;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = previewFrame_.width;
            desc.Height = previewFrame_.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_d3d.device->CreateTexture2D(&desc, nullptr, previewTex_.put())) ||
                FAILED(g_d3d.device->CreateShaderResourceView(previewTex_.get(), nullptr, previewSrv_.put()))) {
                previewTex_ = nullptr;
                previewSrv_ = nullptr;
                return;
            }
            previewW_ = previewFrame_.width;
            previewH_ = previewFrame_.height;
        }
        g_d3d.context->UpdateSubresource(previewTex_.get(), 0, nullptr, previewFrame_.bgra.data(),
                                         previewFrame_.width * 4, 0);
    }

    // Комбо выбора устройства; id пустой — «по умолчанию».
    static void DeviceCombo(const char* id, const std::vector<AudioDevice>& list, std::wstring& selected)
    {
        std::string preview = "По умолчанию";
        for (const auto& d : list)
            if (d.id == selected)
                preview = d.name;
        if (!selected.empty() && preview == "По умолчанию")
            preview = "(отключено) → по умолчанию";
        if (ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("По умолчанию", selected.empty()))
                selected.clear();
            for (size_t i = 0; i < list.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const std::string label = list[i].name + (list[i].isDefault ? "  ★" : "");
                if (ImGui::Selectable(label.c_str(), list[i].id == selected))
                    selected = list[i].id;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    HWND SelectedWindow() const
    {
        return windowSel_ >= 0 && windowSel_ < static_cast<int>(windows_.size())
                   ? windows_[static_cast<size_t>(windowSel_)].hwnd
                   : nullptr;
    }

    static HotkeyBinds BindsOf(const GuiSettings& s)
    {
        HotkeyBinds b;
        b.record = { static_cast<UINT>(s.hkRecordVk), static_cast<UINT>(s.hkRecordMods) };
        b.replay = { static_cast<UINT>(s.hkReplayVk), static_cast<UINT>(s.hkReplayMods) };
        b.screenshot = { static_cast<UINT>(s.hkShotVk), static_cast<UINT>(s.hkShotMods) };
        b.stream = { static_cast<UINT>(s.hkStreamVk), static_cast<UINT>(s.hkStreamMods) };
        b.pause = { static_cast<UINT>(s.hkPauseVk), static_cast<UINT>(s.hkPauseMods) };
        return b;
    }
    HotkeyBinds Binds() const { return BindsOf(s_); }
    // Подпись сочетания. Бинды применяются сразу, без перезапуска захвата.
    std::string KeyHint(HotkeyAction a) const
    {
        const HotkeyBinds b = Binds();
        switch (a) {
        case HotkeyAction::ToggleRecord: return HotkeyName(b.record);
        case HotkeyAction::SaveReplay: return HotkeyName(b.replay);
        case HotkeyAction::Screenshot: return HotkeyName(b.screenshot);
        case HotkeyAction::TogglePause: return HotkeyName(b.pause);
        default: return HotkeyName(b.stream);
        }
    }
    StreamConfig StreamCfg() const
    {
        StreamConfig c;
        c.url = s_.streamUrl;
        c.key = s_.streamKey;
        c.codec = s_.streamCodec == 1 ? Codec::HEVC : Codec::H264;
        c.height = static_cast<uint32_t>(s_.streamHeight);
        c.fps = static_cast<uint32_t>(s_.streamFps);
        c.bitrateKbps = static_cast<uint32_t>(s_.streamBitrateKbps);
        c.reconnect = s_.streamReconnect;
        c.alsoRecord = s_.streamAlsoRecord;
        return c;
    }
    static bool Streaming(const EngineStatus& st)
    {
        return st.streamState == StreamState::Connecting || st.streamState == StreamState::Live ||
               st.streamState == StreamState::Reconnecting;
    }
    float SystemGainLive() const { return s_.systemMuted ? 0.0f : s_.systemGain; }
    float MicGainLive() const { return s_.micMuted ? 0.0f : s_.micGain; }

    // librarySort, громкость и mute меняются на лету — движок из-за них не перезапускается.
    bool SameEngineSettings(const GuiSettings& a, const GuiSettings& b) const
    {
        return a.sourceIsWindow == b.sourceIsWindow && a.monitor == b.monitor && a.codec == b.codec &&
               a.fps == b.fps && a.bitrateKbps == b.bitrateKbps && a.keyintSeconds == b.keyintSeconds &&
               a.cursor == b.cursor && a.systemAudio == b.systemAudio && a.microphone == b.microphone &&
               a.audioBitrateKbps == b.audioBitrateKbps && a.preset == b.preset &&
               a.replaySeconds == b.replaySeconds && a.outDir == b.outDir && a.encoder == b.encoder &&
               a.renderDeviceId == b.renderDeviceId && a.captureDeviceId == b.captureDeviceId &&
               a.overlay == b.overlay && a.outputHeight == b.outputHeight;
    }

    // Кодек, fps и прочее — полный перезапуск. Источник сюда не входит: его меняет SetSource.
    bool NeedsOtherRestart() const
    {
        GuiSettings a = s_;
        GuiSettings b = applied_;
        a.sourceIsWindow = b.sourceIsWindow;
        a.monitor = b.monitor;
        return !SameEngineSettings(a, b);
    }

    bool SourceMismatch() const
    {
        if (s_.sourceIsWindow != applied_.sourceIsWindow)
            return true;
        if (s_.sourceIsWindow)
            return SelectedWindow() != appliedHwnd_;
        return s_.monitor != applied_.monitor;
    }

    SourceSpec CurrentSource() const
    {
        SourceSpec spec;
        if (s_.sourceIsWindow) {
            spec.type = SourceSpec::Type::Window;
            spec.hwnd = SelectedWindow();
            spec.title = s_.windowTitle;
        } else {
            spec.type = SourceSpec::Type::Monitor;
            spec.monitor = s_.monitor;
        }
        return spec;
    }

    // Окно или монитор — сразу, без Join, если захват уже идёт.
    void ApplySourceLive()
    {
        if (s_.sourceIsWindow) {
            if (!SelectedWindow())
                return;
            s_.windowTitle = windows_[static_cast<size_t>(windowSel_)].title;
        }
        applied_.sourceIsWindow = s_.sourceIsWindow;
        applied_.monitor = s_.monitor;
        applied_.windowTitle = s_.windowTitle;
        appliedHwnd_ = s_.sourceIsWindow ? SelectedWindow() : nullptr;
        sourceError_.clear();
        const bool alive = engine_ && engine_->Status().running;
        if (alive)
            engine_->SetSource(CurrentSource());
        else {
            StartEngine();
            return;
        }
        SaveSettings(s_);
        saved_ = s_;
    }

    void StartEngine()
    {
        if (s_.sourceIsWindow && !SelectedWindow()) {
            sourceError_ = "Выберите окно для захвата";
            return;
        }
        StopEngine();
        sourceError_.clear();
        applied_ = s_;
        appliedHwnd_ = SelectedWindow();

        EngineConfig c;
        if (s_.sourceIsWindow) {
            c.source.type = SourceSpec::Type::Window;
            c.source.hwnd = appliedHwnd_;
            s_.windowTitle = windows_[static_cast<size_t>(windowSel_)].title;
            applied_.windowTitle = s_.windowTitle;
        } else {
            c.source.type = SourceSpec::Type::Monitor;
            c.source.monitor = s_.monitor;
        }
        c.codec = s_.codec == 1 ? Codec::HEVC : Codec::H264;
        constexpr EncoderPref kPrefs[] = { EncoderPref::Auto, EncoderPref::Nvenc, EncoderPref::Amf, EncoderPref::Qsv };
        c.encoder = kPrefs[std::clamp(s_.encoder, 0, 3)];
        c.outputHeight = static_cast<uint32_t>(s_.outputHeight);
        c.renderDeviceId = s_.renderDeviceId;
        c.captureDeviceId = s_.captureDeviceId;
        c.overlay = s_.overlay;
        c.fps = static_cast<uint32_t>(s_.fps);
        c.bitrateKbps = static_cast<uint32_t>(s_.bitrateKbps);
        c.keyintSeconds = static_cast<uint32_t>(s_.keyintSeconds);
        c.cursor = s_.cursor;
        c.systemAudio = s_.systemAudio;
        c.systemGain = SystemGainLive();
        c.microphone = s_.microphone;
        c.micGain = MicGainLive();
        c.audioBitrateKbps = static_cast<uint32_t>(s_.audioBitrateKbps);
        c.replaySeconds = static_cast<uint32_t>(s_.replaySeconds);
        c.outDir = s_.outDir;
        c.hotkeys = true;
        c.hotkeyBinds = Binds();
        c.lowLatency = s_.preset == 0;

        engine_ = std::make_unique<RecorderEngine>(std::move(c));
        engine_->StartThread();
        pushedBinds_ = Binds();
        bindsPushed_ = true;
        SaveSettings(s_);
        saved_ = s_;
    }

    void StopEngine()
    {
        if (engine_) {
            engine_->RequestStop();
            engine_->Join();
            engine_.reset();
        }
    }

    void DrawHeader(const EngineStatus& st, ImFont* bold);
    void DrawLibrary(const EngineStatus& st);
    void DrawTile(const RecordingItem& item, float width, bool busy);
    void DrawLog();
    void DrawModeTabs(bool locked);
    void DrawSource(bool locked);
    void DrawVideo(const EngineStatus& st);
    void DrawAudio();
    void DrawReplay();
    void DrawHotkeys(const EngineStatus& st);
    void DrawStream(const EngineStatus& st);
    void DrawStreamWidgets(const EngineStatus& st);
    void DrawLiveButton(const EngineStatus& st);
    void DrawWebcam();
    void DrawWebcamWindow(const EngineStatus& st);
    void DrawRecordBlock(const EngineStatus& st, ImFont* bold);
    bool RecButton(const EngineStatus& st, ImFont* bold, float radius);
    void DrawFooter(const EngineStatus& st);
    void DrawRecoveredBanner();
    void DrawProductSettings();
    void CheckForUpdates();

    HWND hwnd_;
    GuiSettings s_;
    GuiSettings saved_;
    GuiSettings applied_;
    const char* tip_ = "";
    HotkeyBinds pushedBinds_{};
    bool bindsPushed_ = false;
    HWND appliedHwnd_ = nullptr;
    uint64_t windowRefreshTick_ = 0;
    std::vector<MonitorEntry> monitors_;
    std::vector<WindowEntry> windows_;
    int windowSel_ = -1;
    std::vector<AudioDevice> renderDevices_;
    std::vector<AudioDevice> captureDevices_;
    std::unique_ptr<RecorderEngine> engine_;
    std::vector<std::wstring> restored_;
    std::string sourceError_;
    bool locked_ = false;  // идёт запись: настройки с перезапуском движка заблокированы
    bool settingsOpen_ = false;
    std::mutex updateMtx_;
    std::thread updateThread_;
    bool updateBusy_ = false;
    std::string updateNote_;
    char urlBuf_[512]{};
    char keyBuf_[256]{};
    bool showKey_ = false;

    RecordingLibrary library_;
    std::wstring selected_;
    bool logOpen_ = false;

    std::vector<VideoDevice> cameras_;
    std::shared_ptr<WebcamSource> webcam_;
    bool webcamWindow_ = false;
    WebcamFrame previewFrame_;
    uint64_t previewSeq_ = 0;
    winrt::com_ptr<ID3D11Texture2D> previewTex_;
    winrt::com_ptr<ID3D11ShaderResourceView> previewSrv_;
    uint32_t previewW_ = 0, previewH_ = 0;
    enum class Drag { None, Move, TopLeft, TopRight, BottomLeft, BottomRight };
    Drag drag_ = Drag::None;
    WebcamRect dragStart_;
    ImVec2 dragMouse_{};

    UrlImageCache images_;
    char widgetUrl_[kMaxStreamWidgets][512]{};
    struct WidgetPreview {
        uint64_t seq = 0;
        uint32_t w = 0, h = 0;
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
    };
    WidgetPreview widgetPrev_[kMaxStreamWidgets]{};
    int widgetDragSlot_ = -1;
    Drag widgetDrag_ = Drag::None;
    WebcamRect widgetDragStart_{};
    ImVec2 widgetDragMouse_{};

    void RememberCaptionButton(RECT& dst);
    void UploadWidgetPreview(int slot, const UrlBitmap& bm);
};

void App::RememberCaptionButton(RECT& dst)
{
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    dst = { static_cast<LONG>(a.x), static_cast<LONG>(a.y), static_cast<LONG>(b.x), static_cast<LONG>(b.y) };
}

// ---------- Шапка: своя рамка окна + GuVi и подсказка ----------
void App::DrawHeader(const EngineStatus& st, ImFont* bold)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float dpi = style.FontScaleDpi;
    const float side = std::floor(std::max(ImGui::GetFrameHeight(), 28.0f * dpi));
    const float gap = std::floor(6.0f * dpi);
    const float btnRight = std::floor(10.0f * dpi);
    const float buttonsW = side * 4.0f + gap * 3.0f + btnRight;
    const float barH = side + 12.0f * dpi;
    const ImVec2 bar0 = vp->WorkPos;
    ImGui::SetCursorScreenPos(bar0);

    ImGui::PushFont(bold, style.FontSizeBase * 1.35f);
    ImGui::SetCursorScreenPos({ bar0.x + 14.0f * dpi, bar0.y + (barH - ImGui::GetFontSize()) * 0.5f });
    ImGui::TextColored(kAccent, "●");
    ImGui::SameLine();
    ImGui::TextUnformatted(kAppName);
    ImGui::PopFont();

    if (st.streamState == StreamState::Live) {
        ImGui::SameLine();
        ImGui::TextColored(kAccentHover, "● LIVE");
    } else if (Streaming(st)) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "● эфир");
    } else if (st.paused) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "❚❚ пауза");
    }

    const std::string pill = st.ready ? std::format("{}  ·  {}", st.encoderName, st.gpu)
                                      : std::string(st.running ? Tr("запуск захвата…") : Tr("захват остановлен"));
    const ImVec2 pad = style.FramePadding;
    const float pillW = ImGui::CalcTextSize(pill.c_str()).x + pad.x * 2 + ImGui::GetFontSize();
    const float tipMax = vp->WorkSize.x - (ImGui::GetCursorScreenPos().x - bar0.x) - pillW - buttonsW - 24.0f * dpi;
    if (tip_ && tipMax > 40.0f * dpi) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", Ellipsize(Tr(tip_), tipMax).c_str());
    }

    if (pillW + buttonsW + 16.0f * dpi < ImGui::GetContentRegionAvail().x) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - pillW - buttonsW);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 ts = ImGui::CalcTextSize(pill.c_str());
        const ImVec2 size{ pillW, ts.y + pad.y * 2 };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, { p.x + size.x, p.y + size.y }, Col(kTile), size.y * 0.5f);
        const float r = ImGui::GetFontSize() * 0.22f;
        dl->AddCircleFilled({ p.x + pad.x + r, p.y + size.y * 0.5f }, r, Col(st.ready ? kOk : kWarn));
        dl->AddText({ p.x + pad.x + ImGui::GetFontSize(), p.y + pad.y }, ImGui::GetColorU32(ImGuiCol_Text), pill.c_str());
        ImGui::Dummy(size);
    }

    // Три одинаковые кнопки, один зазор, по вертикали по центру шапки — SameLine их съезжает.
    const float yBtn = std::floor(bar0.y + (barH - side) * 0.5f);
    float xBtn = std::floor(bar0.x + vp->WorkSize.x - buttonsW);
    const ImVec2 btnSize{ side, side };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0, 0 });
    ImGui::SetCursorScreenPos({ xBtn, yBtn });
    if (ImGui::Button(Tr("⚙"), btnSize))
        settingsOpen_ = true;
    RememberCaptionButton(g_caption.gear);
    xBtn += side + gap;
    ImGui::SetCursorScreenPos({ xBtn, yBtn });
    if (ImGui::Button("—", btnSize))
        ShowWindow(hwnd_, SW_MINIMIZE);
    RememberCaptionButton(g_caption.minimize);
    xBtn += side + gap;
    ImGui::SetCursorScreenPos({ xBtn, yBtn });
    if (ImGui::Button(IsZoomed(hwnd_) ? "❐" : "□", btnSize))
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
    RememberCaptionButton(g_caption.maximize);
    xBtn += side + gap;
    ImGui::SetCursorScreenPos({ xBtn, yBtn });
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    if (ImGui::Button("✕", btnSize))
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    RememberCaptionButton(g_caption.close);

    g_caption.bar = { static_cast<LONG>(bar0.x), static_cast<LONG>(bar0.y),
                      static_cast<LONG>(bar0.x + vp->WorkSize.x), static_cast<LONG>(bar0.y + barH) };
    g_caption.ready = true;

    ImGui::SetCursorScreenPos({ bar0.x + style.WindowPadding.x, bar0.y + barH + 4.0f * dpi });
}

// ---------- Левая панель: записи ----------
void App::DrawTile(const RecordingItem& item, float width, bool busy)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = style.FramePadding.x * 0.8f;
    const float thumbW = width - pad * 2;
    const float thumbH = std::floor(thumbW * 9.0f / 16.0f);
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 size{ width, pad + thumbH + style.ItemInnerSpacing.y + line * 2 + style.ItemInnerSpacing.y + pad };

    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("tile", size);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        selected_ = item.path;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        PlayFile(hwnd_, item.path);
    if (ImGui::BeginPopupContextItem("ctx")) {
        if (ImGui::MenuItem("Воспроизвести"))
            PlayFile(hwnd_, item.path);
        if (ImGui::MenuItem("Показать в папке"))
            ShowInFolder(hwnd_, item.path);
        ImGui::EndPopup();
    }
    if (hovered)
        ImGui::SetTooltip("%s\n%s · %s\nДвойной клик — воспроизвести", item.name.c_str(), item.dateText.c_str(),
                          FormatBytes(item.size).c_str());

    const bool selected = item.path == selected_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q{ p.x + size.x, p.y + size.y };
    dl->AddRectFilled(p, q, Col(hovered ? kTileHover : kTile), style.ChildRounding);
    if (selected)
        dl->AddRect(p, q, Col(kAccent), style.ChildRounding, 2.0f * style.FontScaleDpi);

    const ImVec2 t0{ p.x + pad, p.y + pad };
    const ImVec2 t1{ t0.x + thumbW, t0.y + thumbH };
    dl->AddRectFilled(t0, t1, IM_COL32(8, 8, 10, 255), style.FrameRounding);
    uint32_t tw = 0, th = 0;
    if (ID3D11ShaderResourceView* srv = busy ? nullptr : library_.Thumbnail(item, &tw, &th)) {
        // Вписываем кадр в 16:9 с полями.
        const float scale = std::min(thumbW / static_cast<float>(tw), thumbH / static_cast<float>(th));
        const float iw = tw * scale, ih = th * scale;
        const ImVec2 i0{ t0.x + (thumbW - iw) * 0.5f, t0.y + (thumbH - ih) * 0.5f };
        dl->AddImageRounded(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), i0, { i0.x + iw, i0.y + ih }, { 0, 0 },
                            { 1, 1 }, IM_COL32_WHITE, style.FrameRounding);
    } else {
        // Заглушка: треугольник «play».
        const ImVec2 c{ (t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f };
        const float r = thumbH * 0.16f;
        dl->AddCircleFilled(c, r * 1.5f, IM_COL32(40, 40, 46, 255), 32);
        dl->AddTriangleFilled({ c.x - r * 0.45f, c.y - r * 0.7f }, { c.x - r * 0.45f, c.y + r * 0.7f },
                              { c.x + r * 0.75f, c.y }, IM_COL32(150, 150, 160, 255));
    }
    if (busy) {
        const char* badge = "● REC";
        const ImVec2 bs = ImGui::CalcTextSize(badge);
        const ImVec2 b0{ t0.x + pad, t0.y + pad };
        dl->AddRectFilled(b0, { b0.x + bs.x + pad * 2, b0.y + bs.y + pad }, Col(kAccent), style.FrameRounding);
        dl->AddText({ b0.x + pad, b0.y + pad * 0.5f }, IM_COL32_WHITE, badge);
    }

    const float ty = t1.y + style.ItemInnerSpacing.y;
    dl->AddText({ t0.x, ty }, ImGui::GetColorU32(ImGuiCol_Text), Ellipsize(item.name, thumbW).c_str());
    const std::string meta = std::format("{}  ·  {}", item.dateText, FormatBytes(item.size));
    dl->AddText({ t0.x, ty + line + style.ItemInnerSpacing.y * 0.5f }, Col(kMuted), Ellipsize(meta, thumbW).c_str());
}

void App::DrawLibrary(const EngineStatus& st)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::wstring busy = st.recording ? st.recordPath : std::wstring();
    library_.Tick(g_d3d.device.get(), s_.outDir, busy);

    ImGui::SeparatorText(Tr("ЗАПИСИ"));

    // Папка
    ImGui::BeginDisabled(st.recording);
    const float buttons = ImGui::CalcTextSize("Обзор…").x + ImGui::CalcTextSize("Открыть").x +
                          style.FramePadding.x * 4 + style.ItemSpacing.x * 2;
    std::string folder = WideToUtf8(s_.outDir);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons);
    ImGui::InputText("##folder", folder.data(), folder.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Обзор…")) {
        const auto picked = PickFolder(hwnd_, s_.outDir);
        if (!picked.empty())
            s_.outDir = picked;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Открыть")) {
        CreateDirectoryW(s_.outDir.c_str(), nullptr);
        ShellExecuteW(hwnd_, L"open", s_.outDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // Панель: сортировка, действия над выбранным, свободное место
    const auto& items = library_.Items(s_.librarySort);
    const RecordingItem* sel = nullptr;
    for (const auto& it : items)
        if (it.path == selected_)
            sel = &it;

    const float sortW = ImGui::CalcTextSize("Сначала старые").x + ImGui::GetFrameHeight() + style.FramePadding.x * 2;
    ImGui::SetNextItemWidth(std::min(sortW, std::max(80.0f * style.FontScaleDpi, ImGui::GetContentRegionAvail().x)));
    const char* sortTr[4];
    for (int i = 0; i < 4; ++i)
        sortTr[i] = Tr(kSortNames[i]);
    if (ImGui::Combo("##sort", &s_.librarySort, sortTr, 4))
        SaveSettings(s_);
    const auto placeIfFits = [&](const char* label) {
        const float w = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2;
        if (ImGui::GetContentRegionAvail().x >= w + style.ItemSpacing.x)
            ImGui::SameLine();
    };
    placeIfFits("▶  Воспроизвести");
    ImGui::BeginDisabled(!sel);
    if (AccentButton("▶  Воспроизвести") && sel)
        PlayFile(hwnd_, sel->path);
    placeIfFits("Показать в папке");
    if (ImGui::Button("Показать в папке") && sel)
        ShowInFolder(hwnd_, sel->path);
    ImGui::EndDisabled();

    const std::string disk = library_.TotalBytes()
                                 ? std::format("Свободно {} из {}", FormatBytes(library_.FreeBytes()),
                                               FormatBytes(library_.TotalBytes()))
                                 : std::string("Диск недоступен");
    const float diskW = ImGui::CalcTextSize(disk.c_str()).x;
    if (ImGui::GetContentRegionAvail().x >= diskW + style.ItemSpacing.x) {
        ImGui::SameLine();
        const float slack = ImGui::GetContentRegionAvail().x - diskW;
        if (slack > 0)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + slack);
    } else {
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, avail - diskW));
    }
    ImGui::AlignTextToFramePadding();
    const bool lowDisk = library_.TotalBytes() && library_.FreeBytes() < 5ull * 1024 * 1024 * 1024;
    ImGui::TextColored(lowDisk ? kWarn : kMuted, "%s", disk.c_str());

    // Полоса заполнения диска
    if (library_.TotalBytes()) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = 3.0f * style.FontScaleDpi;
        const float used = 1.0f - static_cast<float>(static_cast<double>(library_.FreeBytes()) / library_.TotalBytes());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, { p.x + w, p.y + h }, Col(kTile), h);
        dl->AddRectFilled(p, { p.x + w * used, p.y + h }, Col(lowDisk ? kWarn : kAccent, 0.8f), h);
        ImGui::Dummy({ w, h });
    }

    // Сетка миниатюр
    const float logH = logOpen_ ? ImGui::GetTextLineHeightWithSpacing() * 8 + ImGui::GetFrameHeightWithSpacing()
                                : ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{ 0, 0, 0, 0 });
    ImGui::BeginChild("grid", { 0, -logH }, ImGuiChildFlags_None);
    if (items.empty()) {
        ImGui::Dummy({ 0, ImGui::GetContentRegionAvail().y * 0.35f });
        const char* t1 = "Здесь появятся ваши записи";
        const std::string t2s = "Нажмите REC или " + KeyHint(HotkeyAction::ToggleRecord);
        const char* t2 = t2s.c_str();
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(t1).x) * 0.5f);
        ImGui::TextDisabled("%s", t1);
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(t2).x) * 0.5f);
        ImGui::TextDisabled("%s", t2);
    } else {
        const float spacing = style.ItemSpacing.x;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float minTile = 200.0f * style.FontScaleDpi;
        const int cols = std::max(1, static_cast<int>((avail + spacing) / (minTile + spacing)));
        const float tileW = std::floor((avail - spacing * (cols - 1)) / cols);
        for (size_t i = 0; i < items.size(); ++i) {
            if (i % cols != 0)
                ImGui::SameLine(0, spacing);
            ImGui::PushID(static_cast<int>(i));
            DrawTile(items[i], tileW, items[i].path == busy);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    DrawLog();
}

void App::DrawLog()
{
    ImGui::SetNextItemOpen(logOpen_);
    logOpen_ = ImGui::CollapsingHeader("Журнал");
    if (!logOpen_)
        return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("log", { -FLT_MIN, -FLT_MIN }, ImGuiChildFlags_None);
    static uint64_t lastVersion = 0;
    const auto lines = RecentLog(60);
    for (const auto& l : lines) {
        const ImVec4 col = l.level == LogLevel::Error  ? kAccentHover
                         : l.level == LogLevel::Warn   ? kWarn
                                                       : ImVec4{ 0.72f, 0.73f, 0.77f, 1.0f };
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s", l.text.c_str());
        ImGui::PopStyleColor();
    }
    const uint64_t v = LogVersion();
    if (v != lastVersion) {
        ImGui::SetScrollHereY(1.0f);
        lastVersion = v;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---------- Правая панель: настройки и запись ----------
// Строка источника: быстрый выбор Экран / Окно и переключатель веб-камеры.
void App::DrawModeTabs(bool locked)
{
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float w = (ImGui::GetContentRegionAvail().x - spacing * 2) / 3;
    const char* labels[] = { Tr("Экран"), Tr("Окно") };
    const char* tips[] = { "Запись монитора целиком (игры в borderless/полноэкранном окне — тоже)",
                           "Запись одного окна" };
    for (int i = 0; i < 2; ++i) {
        if (i)
            ImGui::SameLine(0, spacing);
        const bool active = (i == 1) == s_.sourceIsWindow;
        ImGui::BeginDisabled(locked && !active);
        if (active ? AccentButton(labels[i], { w, 0 }) : ImGui::Button(labels[i], { w, 0 }))
            s_.sourceIsWindow = i == 1;
        ImGui::EndDisabled();
        HoverTip(tips[i]);
    }
    ImGui::SameLine(0, spacing);
    if (s_.webcam ? AccentButton("● Камера", { w, 0 }) : ImGui::Button("○ Камера", { w, 0 })) {
        s_.webcam = !s_.webcam;
        SaveSettings(s_);
    }
    HoverTip("Веб-камера поверх видео — включается сразу, и во время записи тоже");
}

void App::DrawSource(bool locked)
{
    ImGui::BeginDisabled(locked);
    const float refreshW = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - refreshW - ImGui::GetStyle().ItemInnerSpacing.x);
    if (!s_.sourceIsWindow) {
        auto label = [&](const MonitorEntry& m) {
            return std::format("Монитор {} — {}×{}{}", m.index + 1, m.width, m.height, m.primary ? " (основной)" : "");
        };
        const std::string preview = s_.monitor < static_cast<int>(monitors_.size())
                                        ? label(monitors_[static_cast<size_t>(s_.monitor)])
                                        : std::string("нет мониторов");
        if (ImGui::BeginCombo("##monitor", preview.c_str())) {
            for (const auto& m : monitors_) {
                const bool sel = m.index == s_.monitor;
                if (ImGui::Selectable(label(m).c_str(), sel))
                    s_.monitor = m.index;
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        const std::string preview = windowSel_ >= 0 ? WideToUtf8(windows_[static_cast<size_t>(windowSel_)].title)
                                                    : std::string("— выберите окно —");
        if (ImGui::BeginCombo("##window", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (size_t i = 0; i < windows_.size(); ++i) {
                const bool sel = static_cast<int>(i) == windowSel_;
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(WideToUtf8(windows_[i].title).c_str(), sel))
                    windowSel_ = static_cast<int>(i);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::Button("↻", { refreshW, 0 }))
        RefreshSources();
    HoverTip("Обновить список мониторов, окон и аудиоустройств");
    ImGui::EndDisabled();
}

// Две колонки «подпись — значение» внутри правой панели.
void Label(const char* text)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", text);
    ImGui::SameLine(ImGui::GetStyle().FontScaleDpi * 128.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void App::DrawVideo(const EngineStatus& st)
{
    ImGui::SeparatorText(Tr("КОДИРОВАНИЕ"));

    Label("Формат");
    int format = 0;
    const char* formats[] = { "MP4" };
    ImGui::Combo("##format", &format, formats, 1);

    Label("Кодек");
    const char* codecs[] = { "H.264 (AVC)", "HEVC (H.265)" };
    ImGui::Combo("##codec", &s_.codec, codecs, 2);

    Label("Кодировщик");
    const char* encoders[] = { "Авто (по видеокарте)", "NVIDIA NVENC", "AMD AMF", "Intel Quick Sync" };
    ImGui::Combo("##encoder", &s_.encoder, encoders, 4);
    Label("Активен");
    if (st.ready)
        ImGui::TextColored(kOk, "%s", st.encoderName.c_str());
    else
        ImGui::TextDisabled("—");

    Label("Пресет");
    const char* presets[] = { "Низкая задержка", "Качество" };
    ImGui::Combo("##preset", &s_.preset, presets, 2);
    HoverTip("Низкая задержка — меньше всего нагрузки на GPU, когда игра грузит видеокарту (NVENC P2, ultra low latency).\n"
             "Качество — лучше картинка при том же битрейте, но кодировщик работает дольше (NVENC P5, high quality).");

    Label("Разрешение");
    int hIdx = 0;
    for (int i = 0; i < static_cast<int>(std::size(kHeights)); ++i)
        if (kHeights[i] == s_.outputHeight)
            hIdx = i;
    const char* heightTr[std::size(kHeightNames)];
    for (size_t i = 0; i < std::size(kHeightNames); ++i)
        heightTr[i] = Tr(kHeightNames[i]);
    if (ImGui::Combo("##height", &hIdx, heightTr, static_cast<int>(std::size(kHeightNames))))
        s_.outputHeight = kHeights[hIdx];
    HoverTip("Только уменьшение, с сохранением пропорций; если источник меньше — пишется как есть");

    Label("FPS");
    const std::string fpsPreview = std::to_string(s_.fps);
    if (ImGui::BeginCombo("##fps", fpsPreview.c_str())) {
        for (const int f : kFpsPresets)
            if (ImGui::Selectable(std::to_string(f).c_str(), f == s_.fps))
                s_.fps = f;
        ImGui::EndCombo();
    }

    Label("Битрейт");
    ImGui::SliderInt("##bitrate", &s_.bitrateKbps, 2000, 100000, "%d кбит/с", ImGuiSliderFlags_Logarithmic);

    Label("Ключевой кадр");
    ImGui::SliderInt("##keyint", &s_.keyintSeconds, 1, 10, "каждые %d с");

    Label("Курсор");
    ImGui::Checkbox("##cursor", &s_.cursor);
}

void App::DrawAudio()
{
    ImGui::SeparatorText(Tr("ЗВУК"));

    // Строка источника: флажок дорожки + устройство (с перезапуском), под ней mute и громкость (на лету).
    const auto row = [&](const char* label, const char* id, bool& enabled, const std::vector<AudioDevice>& list,
                         std::wstring& device, bool& muted, float& gain, int maxPct) {
        ImGui::PushID(id);
        Label(label);
        ImGui::BeginDisabled(locked_);
        ImGui::Checkbox("##on", &enabled);
        ImGui::SameLine();
        ImGui::BeginDisabled(!enabled);
        ImGui::SetNextItemWidth(-FLT_MIN);
        DeviceCombo("##dev", list, device);
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::SetCursorPosX(ImGui::GetStyle().FontScaleDpi * 128.0f);
        ImGui::BeginDisabled(!enabled);
        if (MuteButton("##mute", muted))
            SaveSettings(s_);
        HoverTip(muted ? "Звук выключен — нажмите, чтобы включить" : "Выключить звук (mute)");
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(-FLT_MIN);
        int pct = static_cast<int>(std::lround(gain * 100));
        ImGui::BeginDisabled(muted);
        if (ImGui::SliderInt("##gain", &pct, 0, maxPct, muted ? "без звука" : "%d %%"))
            gain = pct / 100.0f;
        if (ImGui::IsItemDeactivatedAfterEdit())
            SaveSettings(s_);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopID();
    };
    row("Система", "sys", s_.systemAudio, renderDevices_, s_.renderDeviceId, s_.systemMuted, s_.systemGain, 200);
    row("Микрофон", "mic", s_.microphone, captureDevices_, s_.captureDeviceId, s_.micMuted, s_.micGain, 400);
    ImGui::BeginDisabled(locked_);

    Label("AAC");
    const std::string aacPreview = std::format("{} кбит/с", s_.audioBitrateKbps);
    ImGui::BeginDisabled(!s_.systemAudio && !s_.microphone);
    if (ImGui::BeginCombo("##aac", aacPreview.c_str())) {
        for (const int r : kAacRates)
            if (ImGui::Selectable(std::format("{} кбит/с", r).c_str(), r == s_.audioBitrateKbps))
                s_.audioBitrateKbps = r;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

void App::DrawHotkeys(const EngineStatus& st)
{
    ImGui::SeparatorText(Tr("ГОРЯЧИЕ КЛАВИШИ"));
    struct Row {
        const char* label;
        int* vk;
        int* mods;
        HotkeyAction action;
    };
    const Row rows[] = {
        { "Старт/стоп", &s_.hkRecordVk, &s_.hkRecordMods, HotkeyAction::ToggleRecord },
        { "Повтор", &s_.hkReplayVk, &s_.hkReplayMods, HotkeyAction::SaveReplay },
        { "Скриншот", &s_.hkShotVk, &s_.hkShotMods, HotkeyAction::Screenshot },
        { "Трансляция", &s_.hkStreamVk, &s_.hkStreamMods, HotkeyAction::ToggleStream },
        { "Пауза", &s_.hkPauseVk, &s_.hkPauseMods, HotkeyAction::TogglePause },
    };

    if (g_keyCapture.target >= 0 && g_keyCapture.done) {
        const Row& r = rows[g_keyCapture.target];
        if (g_keyCapture.clear) {
            *r.vk = 0;
            *r.mods = 0;
        } else if (!g_keyCapture.cancel) {
            *r.vk = static_cast<int>(g_keyCapture.vk);
            *r.mods = static_cast<int>(g_keyCapture.mods);
            // То же сочетание у другого действия — снимаем его там.
            for (const Row& o : rows)
                if (&o != &r && *o.vk == *r.vk && *o.mods == *r.mods)
                    *o.vk = *o.mods = 0;
        }
        g_keyCapture = {};
        SaveSettings(s_);
    }

    for (int i = 0; i < static_cast<int>(std::size(rows)); ++i) {
        const Row& r = rows[i];
        ImGui::PushID(i);
        Label(r.label);
        const bool capturing = g_keyCapture.target == i;
        const std::string text =
            capturing ? std::string("Нажмите сочетание…")
                      : HotkeyName({ static_cast<UINT>(*r.vk), static_cast<UINT>(*r.mods) });
        if (capturing ? AccentButton(text.c_str(), { -FLT_MIN, 0 }) : ImGui::Button(text.c_str(), { -FLT_MIN, 0 })) {
            g_keyCapture = {};
            g_keyCapture.target = capturing ? -1 : i;
        }
        HoverTip("Клик — назначить: клавиша или кнопка мыши (Mouse4 — боковая).\nМожно с Ctrl / Alt / Shift. Esc — отмена, Backspace — убрать.");
        if (st.ready && (st.hotkeysFailed & (1u << static_cast<int>(r.action)))) {
            ImGui::SetCursorPosX(ImGui::GetStyle().FontScaleDpi * 128.0f);
            ImGui::TextColored(kWarn, "занято другой программой — выберите другое");
        }
        ImGui::PopID();
    }
}

void App::DrawReplay()
{
    ImGui::SeparatorText(Tr("ПОВТОР"));
    Label("Длина повтора");
    ImGui::SliderInt("##replay", &s_.replaySeconds, 0, 300, s_.replaySeconds ? "%d с" : "выключен");
    HoverTip("Instant replay: последние N секунд держатся в памяти, горячая клавиша «Повтор» сохраняет их в файл");

    Label("HUD на экране");
    ImGui::Checkbox("##hud", &s_.overlay);
    HoverTip("Плашка «ЗАПИСЬ» в правом верхнем углу. Видна только вам: исключена из захвата и в видео не попадает");
}

void App::DrawWebcam()
{
    ImGui::SeparatorText(Tr("ВЕБ-КАМЕРА"));
    const ImGuiStyle& style = ImGui::GetStyle();

    Label("Камера");
    const float gear = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - gear - style.ItemInnerSpacing.x);
    std::string preview = "Первая доступная";
    for (const auto& c : cameras_)
        if (c.id == s_.webcamDeviceId)
            preview = c.name;
    if (!s_.webcamDeviceId.empty() && preview == "Первая доступная")
        preview = "(отключена) — выберите другую";
    ImGui::BeginDisabled(!s_.webcam);
    if (ImGui::BeginCombo("##camera", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("Первая доступная", s_.webcamDeviceId.empty())) {
            s_.webcamDeviceId.clear();
            SaveSettings(s_);
        }
        for (size_t i = 0; i < cameras_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string label = cameras_[i].name + (cameras_[i].directShow ? "  (DirectShow)" : "");
            if (ImGui::Selectable(label.c_str(), cameras_[i].id == s_.webcamDeviceId)) {
                s_.webcamDeviceId = cameras_[i].id;
                SaveSettings(s_);
            }
            ImGui::PopID();
        }
        if (cameras_.empty())
            ImGui::TextDisabled("Камеры не найдены — нажмите ↻");
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, style.ItemInnerSpacing.x);
    if (ImGui::Button("⚙", { gear, 0 }))
        webcamWindow_ = true;
    HoverTip("Настройки веб-камеры: размер и положение в кадре");

    Label("Режим");
    int mode = s_.webcamMode;
    const char* modes[] = { "Всегда записывать" };
    ImGui::BeginDisabled(!s_.webcam);
    ImGui::Combo("##webcammode", &mode, modes, 1);
    ImGui::EndDisabled();

    if (s_.webcam && webcam_) {
        ImGui::SetCursorPosX(style.FontScaleDpi * 128.0f);
        ImGui::TextColored(webcam_->Failed() ? kWarn : kMuted, "%s", webcam_->Status().c_str());
    }
}

void App::DrawWebcamWindow(const EngineStatus& st)
{
    if (!webcamWindow_)
        return;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float dpi = style.FontScaleDpi;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({ vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f },
                            ImGuiCond_Appearing, { 0.5f, 0.5f });
    ImGui::SetNextWindowSize({ 640 * dpi, 0 }, ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * dpi);
    const bool wasOpen = webcamWindow_;
    const bool visible = ImGui::Begin("Настройки веб-камеры", &webcamWindow_,
                                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar(2);
    if (!visible) {
        ImGui::End();
        return;
    }

    UpdatePreview();

    // Холст с пропорциями записываемого кадра.
    const float frameW = st.videoWidth ? static_cast<float>(st.videoWidth) : 1920.0f;
    const float frameH = st.videoHeight ? static_cast<float>(st.videoHeight) : 1080.0f;
    const float canvasW = 600.0f * dpi;
    const float canvasH = std::floor(canvasW * frameH / frameW);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1{ p0.x + canvasW, p0.y + canvasH };

    ImGui::InvisibleButton("##canvas", { canvasW, canvasH });
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool activated = ImGui::IsItemActivated();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, p1, true);
    const float cell = 12.0f * dpi;
    for (int row = 0; p0.y + row * cell < p1.y; ++row)
        for (int col = 0; p0.x + col * cell < p1.x; ++col) {
            const ImVec2 a{ p0.x + col * cell, p0.y + row * cell };
            dl->AddRectFilled(a, { std::min(a.x + cell, p1.x), std::min(a.y + cell, p1.y) },
                              ((row + col) & 1) ? IM_COL32(58, 58, 64, 255) : IM_COL32(38, 38, 43, 255));
        }

    const bool haveFrame = webcam_ && previewSrv_;
    const float camW = haveFrame ? static_cast<float>(previewW_) : 16.0f;
    const float camH = haveFrame ? static_cast<float>(previewH_) : 9.0f;
    const float aspect = camH / camW;
    // Прямоугольник в координатах холста; в кадр он переносится в процентах, так что масштаб холста не важен.
    WebcamRect r = PlaceWebcam(canvasW, canvasH, camW, camH, Layout());

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float mx = mouse.x - p0.x, my = mouse.y - p0.y;
    const float handle = 7.0f * dpi;
    auto nearPt = [&](float x, float y) { return std::abs(mx - x) <= handle * 1.5f && std::abs(my - y) <= handle * 1.5f; };
    Drag hover = Drag::None;
    if (nearPt(r.x, r.y))
        hover = Drag::TopLeft;
    else if (nearPt(r.x + r.w, r.y))
        hover = Drag::TopRight;
    else if (nearPt(r.x, r.y + r.h))
        hover = Drag::BottomLeft;
    else if (nearPt(r.x + r.w, r.y + r.h))
        hover = Drag::BottomRight;
    else if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h)
        hover = Drag::Move;

    if (activated) {
        drag_ = hover;
        dragStart_ = r;
        dragMouse_ = mouse;
    }
    if (active && drag_ != Drag::None) {
        WebcamRect n = dragStart_;
        if (drag_ == Drag::Move) {
            n.x = std::clamp(dragStart_.x + (mouse.x - dragMouse_.x), 0.0f, canvasW - n.w);
            n.y = std::clamp(dragStart_.y + (mouse.y - dragMouse_.y), 0.0f, canvasH - n.h);
        } else {
            // Противоположный угол стоит на месте, пропорции камеры сохраняются.
            const bool left = drag_ == Drag::TopLeft || drag_ == Drag::BottomLeft;
            const bool top = drag_ == Drag::TopLeft || drag_ == Drag::TopRight;
            const float ax = left ? dragStart_.x + dragStart_.w : dragStart_.x;
            const float ay = top ? dragStart_.y + dragStart_.h : dragStart_.y;
            const float dx = left ? ax - mx : mx - ax;
            const float dy = top ? ay - my : my - ay;
            const float availW = left ? ax : canvasW - ax;
            const float availH = top ? ay : canvasH - ay;
            const float minW = canvasW * kWebcamMinSize / 100.0f;
            const float maxW = std::max(minW, std::min(availW, availH / aspect));
            n.w = std::clamp(std::max(dx, dy / aspect), minW, maxW);
            n.h = n.w * aspect;
            n.x = left ? ax - n.w : ax;
            n.y = top ? ay - n.h : ay;
        }
        const WebcamLayout l = LayoutFromRect(canvasW, canvasH, n);
        s_.webcamSize = l.size;
        s_.webcamX = l.x;
        s_.webcamY = l.y;
        r = PlaceWebcam(canvasW, canvasH, camW, camH, Layout());
    }
    if (!active && drag_ != Drag::None) {
        drag_ = Drag::None;
        SaveSettings(s_);
    }

    const Drag cursorFor = active ? drag_ : (hovered ? hover : Drag::None);
    if (cursorFor == Drag::Move)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    else if (cursorFor == Drag::TopLeft || cursorFor == Drag::BottomRight)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if (cursorFor == Drag::TopRight || cursorFor == Drag::BottomLeft)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);

    const ImVec2 r0{ p0.x + r.x, p0.y + r.y };
    const ImVec2 r1{ r0.x + r.w, r0.y + r.h };
    if (haveFrame) {
        dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(previewSrv_.get())), r0, r1);
    } else {
        dl->AddRectFilled(r0, r1, IM_COL32(20, 20, 24, 230));
        const char* text = !s_.webcam ? "Веб-камера выключена" : "Нет сигнала";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        if (ts.x < r.w)
            dl->AddText({ (r0.x + r1.x - ts.x) * 0.5f, (r0.y + r1.y - ts.y) * 0.5f }, Col(kMuted), text);
    }
    const bool highlight = active || hover != Drag::None;
    dl->AddRect(r0, r1, Col(kAccent, highlight ? 1.0f : 0.7f), 0.0f, 2.0f * dpi);
    for (const ImVec2 c : { r0, ImVec2{ r1.x, r0.y }, ImVec2{ r0.x, r1.y }, r1 })
        dl->AddRectFilled({ c.x - handle * 0.5f, c.y - handle * 0.5f }, { c.x + handle * 0.5f, c.y + handle * 0.5f },
                          Col(kAccent), 2.0f * dpi);
    dl->PopClipRect();

    ImGui::TextDisabled("Тяните картинку — переместить, угол — изменить размер.");
    if (webcam_) {
        ImGui::SameLine();
        ImGui::TextColored(webcam_->Failed() ? kWarn : kMuted, " %s%s", webcam_->Status().c_str(),
                           previewFrame_.alpha ? " · с прозрачностью" : "");
    }

    ImGui::Spacing();
    const auto slider = [&](const char* label, const char* id, float& v, float lo, float hi) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(160.0f * dpi);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat(id, &v, lo, hi, "%.0f %%");
        if (ImGui::IsItemDeactivatedAfterEdit())
            SaveSettings(s_);
    };
    slider("Размер видео", "##wsize", s_.webcamSize, kWebcamMinSize, 100.0f);
    slider("По горизонтали", "##wx", s_.webcamX, 0.0f, 100.0f);
    slider("По вертикали", "##wy", s_.webcamY, 0.0f, 100.0f);

    ImGui::Spacing();
    const float doneW = ImGui::CalcTextSize("Готово").x + style.FramePadding.x * 4;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - doneW);
    if (AccentButton("Готово", { doneW, 0 }))
        webcamWindow_ = false;
    ImGui::End();

    if (wasOpen && !webcamWindow_)
        SaveSettings(s_);
}

// Большая круглая красная кнопка. true — нажата.
bool App::RecButton(const EngineStatus& st, ImFont* bold, float radius)
{
    const ImVec2 size{ radius * 2, radius * 2 };
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - size.x) * 0.5f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // Пока окно ещё не отдало кадр, поток движка жив — клик не должен пропадать.
    const bool enabled = !st.stopping && (st.ready || st.running);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##rec", size);
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool held = enabled && ImGui::IsItemActive();
    const std::string recTip =
        std::format("{} ({})", st.recording ? "Остановить запись" : "Начать запись", KeyHint(HotkeyAction::ToggleRecord));
    HoverTip(recTip.c_str());

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c{ p.x + radius, p.y + radius };
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float t = static_cast<float>(ImGui::GetTime());

    if (st.recording) {
        const float pulse = 0.5f + 0.5f * std::sin(t * 4.0f);
        dl->AddCircleFilled(c, radius, Col(kAccent, 0.10f + 0.15f * pulse), 64);
    }
    dl->AddCircle(c, radius - 2 * dpi, Col(enabled ? kAccent : kMuted, 0.55f), 64, 3 * dpi);
    const ImVec4 fill = !enabled ? ImVec4{ 0.30f, 0.16f, 0.18f, 1.0f }
                      : held     ? kAccentActive
                      : hovered  ? kAccentHover
                                 : kAccent;
    const float inner = radius - 9 * dpi;
    dl->AddCircleFilled(c, inner, Col(fill), 64);

    if (st.recording) {
        const float sq = inner * 0.42f;
        dl->AddRectFilled({ c.x - sq, c.y - sq }, { c.x + sq, c.y + sq }, IM_COL32_WHITE, 4 * dpi);
    } else {
        ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 1.6f);
        const char* text = st.stopping ? "…" : "REC";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), { c.x - ts.x * 0.5f, c.y - ts.y * 0.5f },
                    enabled ? IM_COL32_WHITE : IM_COL32(200, 200, 200, 140), text);
        ImGui::PopFont();
    }
    return pressed && enabled;
}

void App::DrawRecordBlock(const EngineStatus& st, ImFont* bold)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    if (RecButton(st, bold, 48.0f * style.FontScaleDpi) && engine_)
        engine_->ToggleRecord();

    // Строка под кнопкой — явное состояние: ожидание / запись / пауза / сохранение.
    const std::string recKey = KeyHint(HotkeyAction::ToggleRecord);
    std::string line;
    ImVec4 color = kMuted;
    if (st.stopping) {
        line = "Сохранение…";
        color = kWarn;
    } else if (st.paused && st.recording) {
        line = std::format("❚❚  ПАУЗА  {}   ·   кадр удерживается", FormatElapsed(st.recordSeconds));
        color = kWarn;
    } else if (st.recording) {
        line = std::format("●  ЗАПИСЬ  {}   ·   {:.0f} fps", FormatElapsed(st.recordSeconds), st.fps);
        if (!st.sourceHint.empty())
            line += std::string("  ·  ") + Tr(st.sourceHint.c_str());
        color = kAccentHover;
    } else if (st.paused) {
        line = "❚❚  ПАУЗА   ·   эфир держит кадр";
        color = kWarn;
    } else if (!st.sourceHint.empty()) {
        line = Tr(st.sourceHint.c_str());
        color = kWarn;
    } else if (!sourceError_.empty()) {
        line = Tr(sourceError_.c_str());
        color = kWarn;
    } else if (st.ready) {
        line = std::format("○  ОЖИДАНИЕ   ·   {} — старт/стоп", recKey);
        color = kOk;
    } else {
        line = st.running ? "Запуск захвата…" : "Захват остановлен";
    }
    ImGui::PushFont(bold, style.FontSizeBase * (st.recording ? 1.2f : 1.0f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(line.c_str()).x) * 0.5f);
    ImGui::TextColored(color, "%s", line.c_str());
    ImGui::PopFont();

    const bool canPause = st.ready && !st.stopping && (st.recording || st.paused || Streaming(st));
    ImGui::BeginDisabled(!canPause);
    const std::string pauseLabel =
        std::format("{}  {}##pause", st.paused ? "Продолжить" : "Пауза", KeyHint(HotkeyAction::TogglePause));
    if (ImGui::Button(pauseLabel.c_str(), { -FLT_MIN, 0 }) && engine_)
        engine_->TogglePause();
    ImGui::EndDisabled();
    HoverTip("Удержать последний кадр. Новое аудио в файл не пишется, эфир показывает тот же кадр.");

    const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(!st.ready || !st.replayEnabled);
    const std::string replayLabel = std::format("Сохранить повтор  {}##replay", KeyHint(HotkeyAction::SaveReplay));
    if (ImGui::Button(replayLabel.c_str(), { half, 0 }) && engine_)
        engine_->SaveReplay();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!st.ready);
    const std::string shotLabel = std::format("Скриншот  {}##shot", KeyHint(HotkeyAction::Screenshot));
    if (ImGui::Button(shotLabel.c_str(), { half, 0 }) && engine_)
        engine_->Screenshot();
    ImGui::EndDisabled();
}

// ---------- Трансляция ----------
void App::DrawStream(const EngineStatus& st)
{
    ImGui::SeparatorText(Tr("ТРАНСЛЯЦИЯ"));
    const ImGuiStyle& style = ImGui::GetStyle();
    // Во время эфира поля замкнуты: применяются при следующем старте.
    ImGui::BeginDisabled(Streaming(st));
    bool changed = false;

    Label("Сервер");
    if (ImGui::InputTextWithHint("##surl", "rtmp://live.twitch.tv/app", urlBuf_, sizeof(urlBuf_))) {
        s_.streamUrl = urlBuf_;
        changed = true;
    }
    HoverTip("RTMP или RTMPS (TLS): rtmp://live.twitch.tv/app, rtmps://a.rtmps.youtube.com/live2 …");

    Label("Ключ потока");
    const float showW = ImGui::CalcTextSize("Скрыть").x + style.FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - showW - style.ItemInnerSpacing.x);
    if (ImGui::InputTextWithHint("##skey", "ключ из панели площадки", keyBuf_, sizeof(keyBuf_),
                                 showKey_ ? 0 : ImGuiInputTextFlags_Password)) {
        s_.streamKey = keyBuf_;
        changed = true;
    }
    ImGui::SameLine(0, style.ItemInnerSpacing.x);
    if (ImGui::Button(showKey_ ? "Скрыть" : "Показ", { showW, 0 }))
        showKey_ = !showKey_;
    HoverTip("Ключ хранится в recorder_gui.json зашифрованным (DPAPI, только ваш пользователь Windows) и не пишется в журнал");

    Label("Кодек");
    const char* codecs[] = { "H.264 (AVC)", "HEVC (Enhanced RTMP)" };
    changed |= ImGui::Combo("##scodec", &s_.streamCodec, codecs, 2);
    HoverTip("HEVC по RTMP — расширение Enhanced RTMP (YouTube, новые сборки OBS-серверов). Twitch и большинство площадок — H.264");

    Label("Разрешение");
    constexpr int kStreamHeights[] = { 1080, 900, 720, 540, 480, 360 };
    const std::string hPreview = std::format("{}p", s_.streamHeight);
    if (ImGui::BeginCombo("##sheight", hPreview.c_str())) {
        for (const int h : kStreamHeights)
            if (ImGui::Selectable(std::format("{}p", h).c_str(), h == s_.streamHeight)) {
                s_.streamHeight = h;
                changed = true;
            }
        ImGui::EndCombo();
    }
    HoverTip("Уменьшается из кадра записи на GPU; выше разрешения записи не поднимается");

    Label("FPS");
    constexpr int kStreamFps[] = { 30, 48, 60 };
    const std::string fPreview = std::to_string(s_.streamFps);
    if (ImGui::BeginCombo("##sfps", fPreview.c_str())) {
        for (const int f : kStreamFps)
            if (ImGui::Selectable(std::to_string(f).c_str(), f == s_.streamFps)) {
                s_.streamFps = f;
                changed = true;
            }
        ImGui::EndCombo();
    }

    Label("Битрейт");
    ImGui::SliderInt("##sbitrate", &s_.streamBitrateKbps, 500, 12000, "%d кбит/с", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::IsItemDeactivatedAfterEdit();

    Label("Переподключение");
    changed |= ImGui::Checkbox("##sreconnect", &s_.streamReconnect);
    HoverTip("Сеть оборвалась — подключаться снова (2, 4, 8, 15 с), запись на диск при этом не прерывается");

    Label("Писать на диск");
    changed |= ImGui::Checkbox("##salso", &s_.streamAlsoRecord);
    HoverTip("Вместе с эфиром начать локальную запись (live_*.mp4) в полном качестве записи; остановится вместе с эфиром");
    ImGui::EndDisabled();

    if (changed)
        SaveSettings(s_);
}

void App::DrawLiveButton(const EngineStatus& st)
{
    const std::string key = KeyHint(HotkeyAction::ToggleStream);
    std::string label;
    ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_Button];
    switch (st.streamState) {
    case StreamState::Live:
        label = std::format("●  LIVE  {}   ·   {:.0f} кбит/с   —   стоп  {}", FormatElapsed(st.streamSeconds),
                            st.streamKbps, key);
        bg = kAccent;
        break;
    case StreamState::Connecting:
        label = std::format("Подключение…   —   отмена  {}", key);
        bg = ImVec4{ 0.35f, 0.25f, 0.10f, 1.0f };
        break;
    case StreamState::Reconnecting:
        label = std::format("↻  Переподключение…   —   стоп  {}", key);
        bg = ImVec4{ 0.35f, 0.25f, 0.10f, 1.0f };
        break;
    default:
        label = std::format("●  LIVE   —   начать трансляцию  {}", key);
        break;
    }
    if (st.paused && st.streamState == StreamState::Live)
        label += "  ·  пауза";
    label += "##live";
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, st.streamState == StreamState::Live ? kAccentHover : ImVec4{ bg.x + 0.08f, bg.y + 0.06f, bg.z + 0.06f, 1.0f });
    ImGui::BeginDisabled(!st.ready);
    if (ImGui::Button(label.c_str(), { -FLT_MIN, 0 }) && engine_) {
        engine_->SetStreamConfig(StreamCfg());
        engine_->ToggleStream();
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (!st.streamInfo.empty() && Streaming(st))
        HoverTip(st.streamInfo.c_str());
    else if (!st.streamError.empty())
        HoverTip(("Ошибка: " + st.streamError).c_str());
}

void App::DrawRecoveredBanner()
{
    if (restored_.empty())
        return;
    std::wstring names;
    for (size_t i = 0; i < restored_.size(); ++i) {
        if (i)
            names += L", ";
        const auto& p = restored_[i];
        const auto slash = p.find_last_of(L"\\/");
        names += slash == std::wstring::npos ? p : p.substr(slash + 1);
    }
    const std::string msg = std::string(Tr("Видео восстановлено")) + ": " + WideToUtf8(names);
    const float btn = ImGui::GetFrameHeight();
    const float room = ImGui::GetContentRegionAvail().x - btn - ImGui::GetStyle().ItemSpacing.x * 2;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 0.62f, 0.88f, 0.55f, 1.0f });
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Ellipsize(msg, std::max(40.0f, room)).c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", msg.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("OK##restored"))
        restored_.clear();
}

// ---------- Подвал: строка «о программе» и состояние ----------
void App::DrawFooter(const EngineStatus& st)
{
    const std::string about = std::format("{} {}", kAppName, kAppVersion);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", about.c_str());

    std::string text;
    ImVec4 color = kMuted;
    if (!st.sourceHint.empty()) {
        text = Tr(st.sourceHint.c_str());
        color = kWarn;
    } else if (!sourceError_.empty()) {
        text = Tr(sourceError_.c_str());
        color = kWarn;
    } else if (!st.error.empty()) {
        text = "Ошибка: " + st.error;
        color = kAccentHover;
    } else if (!st.streamError.empty() && st.streamState != StreamState::Live) {
        text = "Трансляция: " + st.streamError;
        color = kWarn;
    } else if (st.ready) {
        text = std::format("{}  ·  {:.1f} fps  ·  дубли {}  ·  пропущено {}  ·  звук {}", st.encoder, st.fps,
                           static_cast<unsigned long long>(st.duplicatedFrames),
                           static_cast<unsigned long long>(st.droppedFrames), st.audio ? "AAC" : "нет");
        if (st.replayEnabled)
            text += std::format("  ·  буфер {:.0f} МБ", st.replayMB);
        if (Streaming(st) && !st.streamInfo.empty())
            text += "  ·  эфир " + st.streamInfo;
        if (st.droppedFrames)
            color = kWarn;
    }
    if (text.empty())
        return;
    const float textW = ImGui::CalcTextSize(text.c_str()).x;
    const float room = ImGui::GetContentRegionAvail().x;
    if (room >= textW + ImGui::GetStyle().ItemSpacing.x) {
        ImGui::SameLine();
        const float slack = ImGui::GetContentRegionAvail().x - textW;
        if (slack > 0)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + slack);
    } else {
        text = Ellipsize(text, ImGui::GetContentRegionAvail().x);
    }
    ImGui::TextColored(color, "%s", text.c_str());
    if (ImGui::IsItemHovered() && !st.recordPath.empty())
        ImGui::SetTooltip("%s", WideToUtf8(st.recordPath).c_str());
}

void App::UploadWidgetPreview(int slot, const UrlBitmap& bm)
{
    if (slot < 0 || slot >= kMaxStreamWidgets || !bm.bgra || bm.width == 0 || bm.height == 0)
        return;
    if (bm.bgra->size() < static_cast<size_t>(bm.width) * bm.height * 4)
        return;
    WidgetPreview& p = widgetPrev_[slot];
    if (p.srv && p.seq == bm.seq && p.w == bm.width && p.h == bm.height)
        return;
    p.tex = nullptr;
    p.srv = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = bm.width;
    desc.Height = bm.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_d3d.device->CreateTexture2D(&desc, nullptr, p.tex.put())) ||
        FAILED(g_d3d.device->CreateShaderResourceView(p.tex.get(), nullptr, p.srv.put()))) {
        p.tex = nullptr;
        p.srv = nullptr;
        return;
    }
    g_d3d.context->UpdateSubresource(p.tex.get(), 0, nullptr, bm.bgra->data(), bm.width * 4, 0);
    p.w = bm.width;
    p.h = bm.height;
    p.seq = bm.seq;
}

void App::DrawStreamWidgets(const EngineStatus& st)
{
    ImGui::SeparatorText(Tr("ВИДЖЕТЫ НА ЭФИРЕ"));
    ImGui::TextWrapped("До четырёх ссылок. Тяните картинку, угол меняет размер — как веб-камеру. На GPU, только в эфир.");

    const ImGuiStyle& style = ImGui::GetStyle();
    const float dpi = style.FontScaleDpi;
    const float frameW = st.videoWidth ? static_cast<float>(st.videoWidth) : 1920.0f;
    const float frameH = st.videoHeight ? static_cast<float>(st.videoHeight) : 1080.0f;
    const float canvasW = std::max(40.0f, ImGui::GetContentRegionAvail().x);
    const float canvasH = std::floor(std::min(260.0f * dpi, canvasW * frameH / frameW));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1{ p0.x + canvasW, p0.y + canvasH };

    ImGui::InvisibleButton("##widgetcanvas", { canvasW, canvasH });
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool activated = ImGui::IsItemActivated();

    struct Placed {
        WebcamRect rect;
        float aspect = 16.0f / 9.0f;
        UrlBitmap bm;
    };
    std::vector<Placed> placed(static_cast<size_t>(s_.widgetCount));
    for (int i = 0; i < s_.widgetCount; ++i) {
        placed[static_cast<size_t>(i)].bm = images_.Slot(i, s_.widgets[i].url);
        UploadWidgetPreview(i, placed[static_cast<size_t>(i)].bm);
        const UrlBitmap& bm = placed[static_cast<size_t>(i)].bm;
        const float cw = bm.width ? static_cast<float>(bm.width) : 16.0f;
        const float ch = bm.height ? static_cast<float>(bm.height) : 9.0f;
        placed[static_cast<size_t>(i)].aspect = ch / cw;
        placed[static_cast<size_t>(i)].rect =
            PlaceWebcam(canvasW, canvasH, cw, ch, { s_.widgets[i].size, s_.widgets[i].x, s_.widgets[i].y });
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float mx = mouse.x - p0.x;
    const float my = mouse.y - p0.y;
    const float handle = 7.0f * dpi;
    auto nearPt = [&](float x, float y) {
        return std::abs(mx - x) <= handle * 1.5f && std::abs(my - y) <= handle * 1.5f;
    };
    int hoverSlot = -1;
    Drag hover = Drag::None;
    for (int n = s_.widgetCount - 1; n >= 0; --n) {
        const WebcamRect& r = placed[static_cast<size_t>(n)].rect;
        Drag h = Drag::None;
        if (nearPt(r.x, r.y))
            h = Drag::TopLeft;
        else if (nearPt(r.x + r.w, r.y))
            h = Drag::TopRight;
        else if (nearPt(r.x, r.y + r.h))
            h = Drag::BottomLeft;
        else if (nearPt(r.x + r.w, r.y + r.h))
            h = Drag::BottomRight;
        else if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h)
            h = Drag::Move;
        if (h != Drag::None) {
            hover = h;
            hoverSlot = n;
            break;
        }
    }
    if (activated && hoverSlot >= 0) {
        widgetDragSlot_ = hoverSlot;
        widgetDrag_ = hover;
        widgetDragStart_ = placed[static_cast<size_t>(hoverSlot)].rect;
        widgetDragMouse_ = mouse;
    }
    if (active && widgetDragSlot_ >= 0 && widgetDragSlot_ < s_.widgetCount) {
        const float aspect = placed[static_cast<size_t>(widgetDragSlot_)].aspect;
        WebcamRect n = widgetDragStart_;
        if (widgetDrag_ == Drag::Move) {
            n.x = std::clamp(widgetDragStart_.x + (mouse.x - widgetDragMouse_.x), 0.0f, canvasW - n.w);
            n.y = std::clamp(widgetDragStart_.y + (mouse.y - widgetDragMouse_.y), 0.0f, canvasH - n.h);
        } else if (widgetDrag_ != Drag::None) {
            const bool left = widgetDrag_ == Drag::TopLeft || widgetDrag_ == Drag::BottomLeft;
            const bool top = widgetDrag_ == Drag::TopLeft || widgetDrag_ == Drag::TopRight;
            const float ax = left ? widgetDragStart_.x + widgetDragStart_.w : widgetDragStart_.x;
            const float ay = top ? widgetDragStart_.y + widgetDragStart_.h : widgetDragStart_.y;
            const float dx = left ? ax - mx : mx - ax;
            const float dy = top ? ay - my : my - ay;
            const float availW = left ? ax : canvasW - ax;
            const float availH = top ? ay : canvasH - ay;
            const float minW = canvasW * kWebcamMinSize / 100.0f;
            const float maxW = std::max(minW, std::min(availW, availH / aspect));
            n.w = std::clamp(std::max(dx, dy / aspect), minW, maxW);
            n.h = n.w * aspect;
            n.x = left ? ax - n.w : ax;
            n.y = top ? ay - n.h : ay;
        }
        const WebcamLayout layout = LayoutFromRect(canvasW, canvasH, n);
        s_.widgets[widgetDragSlot_].size = layout.size;
        s_.widgets[widgetDragSlot_].x = layout.x;
        s_.widgets[widgetDragSlot_].y = layout.y;
        placed[static_cast<size_t>(widgetDragSlot_)].rect =
            PlaceWebcam(canvasW, canvasH, aspect > 0 ? 100.0f : 16.0f, aspect > 0 ? 100.0f * aspect : 9.0f, layout);
    }
    if (!active && widgetDragSlot_ >= 0) {
        widgetDragSlot_ = -1;
        widgetDrag_ = Drag::None;
    }

    const Drag cursorFor = active ? widgetDrag_ : (hovered ? hover : Drag::None);
    if (cursorFor == Drag::Move)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    else if (cursorFor == Drag::TopLeft || cursorFor == Drag::BottomRight)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if (cursorFor == Drag::TopRight || cursorFor == Drag::BottomLeft)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, p1, true);
    const float cell = 12.0f * dpi;
    for (int row = 0; p0.y + row * cell < p1.y; ++row)
        for (int col = 0; p0.x + col * cell < p1.x; ++col) {
            const ImVec2 a{ p0.x + col * cell, p0.y + row * cell };
            dl->AddRectFilled(a, { std::min(a.x + cell, p1.x), std::min(a.y + cell, p1.y) },
                              ((row + col) & 1) ? IM_COL32(58, 58, 64, 255) : IM_COL32(38, 38, 43, 255));
        }
    for (int i = 0; i < s_.widgetCount; ++i) {
        const WebcamRect& r = placed[static_cast<size_t>(i)].rect;
        const ImVec2 r0{ p0.x + r.x, p0.y + r.y };
        const ImVec2 r1{ r0.x + r.w, r0.y + r.h };
        const bool on = s_.widgets[i].enabled;
        if (widgetPrev_[i].srv) {
            dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(widgetPrev_[i].srv.get())), r0, r1, { 0, 0 },
                         { 1, 1 }, on ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 90));
        } else {
            dl->AddRectFilled(r0, r1, IM_COL32(20, 20, 24, on ? 230 : 120));
            const char* cap = s_.widgets[i].url.empty() ? "ссылка" : (placed[static_cast<size_t>(i)].bm.pending ? "загрузка…" : "ссылка");
            const ImVec2 ts = ImGui::CalcTextSize(cap);
            if (ts.x + 8.0f < r.w)
                dl->AddText({ (r0.x + r1.x - ts.x) * 0.5f, (r0.y + r1.y - ts.y) * 0.5f }, Col(kMuted), cap);
        }
        const bool hot = widgetDragSlot_ == i || hoverSlot == i;
        dl->AddRect(r0, r1, Col(kAccent, hot ? 1.0f : 0.65f), 0.0f, 2.0f * dpi);
        for (const ImVec2 c : { r0, ImVec2{ r1.x, r0.y }, ImVec2{ r0.x, r1.y }, r1 })
            dl->AddRectFilled({ c.x - handle * 0.5f, c.y - handle * 0.5f }, { c.x + handle * 0.5f, c.y + handle * 0.5f },
                              Col(kAccent), 2.0f * dpi);
    }
    dl->PopClipRect();

    if (s_.widgetCount < kMaxStreamWidgets && ImGui::Button("Добавить ссылку")) {
        StreamWidget& w = s_.widgets[s_.widgetCount];
        w = {};
        w.enabled = true;
        w.size = 26.0f;
        w.x = (s_.widgetCount % 2) ? 100.0f : 0.0f;
        w.y = (s_.widgetCount / 2) ? 100.0f : 0.0f;
        widgetUrl_[s_.widgetCount][0] = '\0';
        ++s_.widgetCount;
    }
    for (int i = 0; i < s_.widgetCount; ++i) {
        ImGui::PushID(i);
        ImGui::Checkbox("##on", &s_.widgets[i].enabled);
        HoverTip("Показывать в эфире");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - style.ItemInnerSpacing.x);
        if (ImGui::InputTextWithHint("##url", "https://… картинка или любая ссылка", widgetUrl_[i],
                                     sizeof(widgetUrl_[i])))
            s_.widgets[i].url = widgetUrl_[i];
        ImGui::SameLine();
        if (ImGui::Button("✕", { ImGui::GetFrameHeight(), 0 })) {
            for (int j = i; j < s_.widgetCount - 1; ++j) {
                s_.widgets[j] = s_.widgets[j + 1];
                strncpy_s(widgetUrl_[j], widgetUrl_[j + 1], _TRUNCATE);
                widgetPrev_[j] = std::move(widgetPrev_[j + 1]);
            }
            s_.widgets[s_.widgetCount - 1] = {};
            widgetUrl_[s_.widgetCount - 1][0] = '\0';
            widgetPrev_[s_.widgetCount - 1] = {};
            --s_.widgetCount;
            widgetDragSlot_ = -1;
            ImGui::PopID();
            break;
        }
        if (!s_.widgets[i].url.empty() && placed[static_cast<size_t>(i)].bm.failed)
            ImGui::TextColored(kWarn, "Не картинка — на эфире будет карточка с адресом");
        else if (placed[static_cast<size_t>(i)].bm.pending)
            ImGui::TextDisabled("Загрузка…");
        ImGui::PopID();
    }
}

void App::Draw(ImFont* bold)
{
    I18nBeginFrame();
    I18nUse(s_.language);
    const EngineStatus st = engine_ ? engine_->Status() : EngineStatus{};
    const bool locked = st.recording;
    const ImGuiStyle& style = ImGui::GetStyle();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawHeader(st, bold);
    DrawRecoveredBanner();

    const float footerH = ImGui::GetFrameHeightWithSpacing();
    const float rightW = std::max(380.0f * style.FontScaleDpi, ImGui::GetContentRegionAvail().x * 0.36f);
    const ImGuiChildFlags pane = ImGuiChildFlags_AlwaysUseWindowPadding;

    ImGui::BeginChild("left", { ImGui::GetContentRegionAvail().x - rightW - style.ItemSpacing.x, -footerH }, pane,
                      ImGuiWindowFlags_NoScrollbar);
    DrawLibrary(st);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("right", { 0, -footerH }, pane, ImGuiWindowFlags_NoScrollbar);
    locked_ = locked;
    if (ImGui::BeginTabBar("panels")) {
        if (ImGui::BeginTabItem(Tr("Запись"))) {
            DrawModeTabs(locked);
            DrawSource(locked);
            const float radius = 48.0f * style.FontScaleDpi;
            const float recBlockH = ImGui::GetFrameHeightWithSpacing() * 2 + radius * 2 + style.ItemSpacing.y +
                                    ImGui::GetTextLineHeightWithSpacing() * 1.4f + style.WindowPadding.y;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{ 0, 0, 0, 0 });
            ImGui::BeginChild("options", { 0, -recBlockH }, ImGuiChildFlags_None);
            ImGui::BeginDisabled(locked);
            DrawVideo(st);
            ImGui::EndDisabled();
            DrawAudio();
            ImGui::BeginDisabled(locked);
            DrawReplay();
            ImGui::EndDisabled();
            DrawHotkeys(st);
            DrawWebcam();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            DrawRecordBlock(st, bold);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(Tr("Эфир"))) {
            const float liveH = ImGui::GetFrameHeightWithSpacing();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{ 0, 0, 0, 0 });
            ImGui::BeginChild("streamopts", { 0, -liveH }, ImGuiChildFlags_None);
            DrawStream(st);
            DrawStreamWidgets(st);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            DrawLiveButton(st);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    DrawFooter(st);
    ImGui::End();
    DrawProductSettings();

    DrawWebcamWindow(st);
    SyncWebcam();
    if (engine_) {
        engine_->SetAudioGains(SystemGainLive(), MicGainLive());
        engine_->SetStreamConfig(StreamCfg());
        const HotkeyBinds binds = Binds();
        if (!bindsPushed_ || !(binds == pushedBinds_)) {
            engine_->SetHotkeyBinds(binds);
            pushedBinds_ = binds;
            bindsPushed_ = true;
        }
        std::vector<OverlaySprite> sprites;
        for (int i = 0; i < s_.widgetCount; ++i) {
            if (!s_.widgets[i].enabled || s_.widgets[i].url.empty())
                continue;
            const UrlBitmap bm = images_.Slot(i, s_.widgets[i].url);
            if (!bm.bgra || bm.width == 0)
                continue;
            OverlaySprite sprite;
            sprite.seq = bm.seq;
            sprite.width = bm.width;
            sprite.height = bm.height;
            sprite.layout = { s_.widgets[i].size, s_.widgets[i].x, s_.widgets[i].y };
            sprite.bgra = bm.bgra;
            sprites.push_back(std::move(sprite));
        }
        engine_->SetStreamOverlays(std::move(sprites));
    }
    if (s_.sourceIsWindow && !ImGui::IsAnyItemActive()) {
        const HWND sel = SelectedWindow();
        if (!sel || !IsWindow(sel)) {
            const uint64_t now = GetTickCount64();
            if (now - windowRefreshTick_ >= 1000) {
                windowRefreshTick_ = now;
                RefreshSources();
            }
        }
    }
    if (!st.recording && !st.stopping && !ImGui::IsAnyItemActive()) {
        if (s_.sourceIsWindow && !SelectedWindow())
            sourceError_ = "Выберите окно для захвата";
        else if (NeedsOtherRestart())
            StartEngine();
        else if (!engine_ || SourceMismatch())
            ApplySourceLive();
    }
    if (!ImGui::IsAnyItemActive() && !(s_ == saved_)) {
        SaveSettings(s_);
        saved_ = s_;
    }
}

void App::CheckForUpdates()
{
    if (updateThread_.joinable()) {
        if (updateBusy_)
            return;
        updateThread_.join();
    }
    {
        std::lock_guard lock(updateMtx_);
        updateBusy_ = true;
        updateNote_ = "Проверка…";
    }
    updateThread_ = std::thread([this] {
        std::string note = "Не удалось проверить обновления";
        HINTERNET session = WinHttpOpen(L"GuVi", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (session) {
            HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (connect) {
                std::wstring path = L"/repos/";
                for (const char* p = kGithubRepo; *p; ++p)
                    path.push_back(static_cast<wchar_t>(*p));
                path += L"/releases/latest";
                HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                const wchar_t* headers = L"User-Agent: GuVi\r\nAccept: application/vnd.github+json\r\n";
                if (request && WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0,
                                                   0, 0) &&
                    WinHttpReceiveResponse(request, nullptr)) {
                    DWORD status = 0;
                    DWORD sz = sizeof(status);
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                    std::string body;
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
                            break;
                        const size_t at = body.size();
                        body.resize(at + avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(request, body.data() + at, avail, &read))
                            break;
                        body.resize(at + read);
                    }
                    if (status == 404)
                        note = "Релизов пока нет";
                    else if (status == 200) {
                        const auto key = body.find("\"tag_name\"");
                        const auto q1 = key == std::string::npos ? std::string::npos : body.find('"', key + 11);
                        const auto q2 = q1 == std::string::npos ? std::string::npos : body.find('"', q1 + 1);
                        std::string tag = q1 != std::string::npos && q2 != std::string::npos
                                              ? body.substr(q1 + 1, q2 - q1 - 1)
                                              : std::string();
                        if (!tag.empty() && tag[0] == 'v')
                            tag.erase(0, 1);
                        if (tag.empty())
                            note = "Ответ GitHub без версии";
                        else if (tag == kAppVersion)
                            note = "Установлена последняя версия";
                        else
                            note = "Доступна версия " + tag;
                    }
                }
                if (request)
                    WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
            }
            WinHttpCloseHandle(session);
        }
        std::lock_guard lock(updateMtx_);
        updateBusy_ = false;
        updateNote_ = std::move(note);
    });
}

void App::DrawProductSettings()
{
    if (!settingsOpen_)
        return;
    ImGui::SetNextWindowSize({ 460.0f * ImGui::GetStyle().FontScaleDpi, 0 }, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr("Настройки GuVi"), &settingsOpen_)) {
        ImGui::End();
        return;
    }
    ImGui::SeparatorText(Tr("Язык"));
    const char* codes[] = { "", "ru", "uk", "en", "pl" };
    const char* names[] = { Tr("Как в Windows"), "Русский", "Українська", "English", "Polski" };
    int sel = 0;
    for (int i = 0; i < 5; ++i)
        if (s_.language == codes[i])
            sel = i;
    if (ImGui::BeginCombo("##lang", names[sel])) {
        for (int i = 0; i < 5; ++i) {
            if (ImGui::Selectable(names[i], sel == i)) {
                s_.language = codes[i];
                I18nUse(s_.language);
                SaveSettings(s_);
                saved_ = s_;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SeparatorText(Tr("Обновления"));
    if (ImGui::Button(Tr("Проверить обновления")))
        CheckForUpdates();
    std::string note;
    {
        std::lock_guard lock(updateMtx_);
        note = updateNote_;
    }
    if (!note.empty())
        ImGui::TextWrapped("%s", Tr(note.c_str()));
    ImGui::TextDisabled("github.com/%s", kGithubRepo);

    ImGui::SeparatorText(Tr("О программе"));
    ImGui::Text("%s %s", kAppName, kAppVersion);
    ImGui::TextWrapped("%s", Tr("GuVi пишет игру, окно или весь экран, с микрофоном и веб-камерой, через NVENC. Четыре языка. Обрыв питания не стирает дубль."));
    ImGui::Text("%s", Tr("Автор: Gubke"));
    ImGui::Text("%s", Tr("Создано 28.10.26"));
    ImGui::Text("%s", Tr("Создано вместе с Cursor"));
    auto link = [](const char* label, const wchar_t* url) {
        if (ImGui::Button(label))
            ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    };
    link(Tr("Портативная сборка"), L"https://github.com/Gubke/guvi/releases/download/v1.0.0/GuVi-1.0.0-portable.zip");
    ImGui::SameLine();
    link(Tr("Установщик"), L"https://github.com/Gubke/guvi/releases/download/v1.0.0/GuVi-Setup.exe");
    link(Tr("YouTube"), L"https://www.youtube.com/@gubke");
    ImGui::SameLine();
    link(Tr("Telegram"), L"https://t.me/negubke");
    ImGui::SameLine();
    link(Tr("Почта"), L"mailto:gubke.info@gmail.com");
    ImGui::SameLine();
    link(Tr("Steam"), L"https://steamcommunity.com/id/gubke/");
    ImGui::TextDisabled("@negubke  ·  gubke.info@gmail.com");
    ImGui::End();
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon)
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"GuViRecorder";
    RegisterClassExW(&wc);

    const int w = static_cast<int>(1180 * dpi);
    const int h = static_cast<int>(780 * dpi);
    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, kAppTitleW, style, CW_USEDEFAULT, CW_USEDEFAULT, w,
                                h, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd || !CreateDeviceD3D(hwnd)) {
        MessageBoxW(nullptr, L"Не удалось создать окно / устройство D3D11", kAppTitleW, MB_ICONERROR);
        return 1;
    }

    // Тёмная системная рамка окна (Windows 10 20H1+ / 11).
    const BOOL dark = TRUE;
    using DwmSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        if (auto set = reinterpret_cast<DwmSetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute")))
            set(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        FreeLibrary(dwm);
    }
    MARGINS margins{ 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIcon));
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyTheme(dpi);

    // Символы ▶ ↻ ★ ● — из Segoe UI Symbol, если их нет в основном шрифте.
    ImFontConfig merge;
    merge.MergeMode = true;
    ImFont* regular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    if (!regular)
        regular = io.Fonts->AddFontDefault();
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 17.0f, &merge);
    ImFont* bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 17.0f);
    if (bold)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 17.0f, &merge);
    else
        bold = regular;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_d3d.device.get(), g_d3d.context.get());

    {
        App app(hwnd);
        bool done = false;
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT)
                    done = true;
            }
            if (done)
                break;

            if (IsIconic(hwnd)) {
                Sleep(50);
                continue;
            }
            if (g_d3d.resizeWidth != 0 && g_d3d.resizeHeight != 0) {
                g_d3d.rtv = nullptr;
                g_d3d.swapChain->ResizeBuffers(0, g_d3d.resizeWidth, g_d3d.resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
                g_d3d.resizeWidth = g_d3d.resizeHeight = 0;
                CreateRenderTarget();
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.Draw(bold);
            ImGui::Render();

            const float clear[4] = { 0.065f, 0.065f, 0.078f, 1.0f };
            ID3D11RenderTargetView* rtv = g_d3d.rtv.get();
            g_d3d.context->OMSetRenderTargets(1, &rtv, nullptr);
            g_d3d.context->ClearRenderTargetView(rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_d3d.swapChain->Present(1, 0);
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_d3d = {};
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
