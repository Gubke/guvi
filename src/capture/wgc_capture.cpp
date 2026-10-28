#include "capture/wgc_capture.h"
#include "common/hr.h"
#include "common/log.h"

#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace {

constexpr auto kPixelFormat = wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr int kPoolBuffers = 2;

std::vector<HMONITOR> EnumMonitors()
{
    std::vector<HMONITOR> list;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            reinterpret_cast<std::vector<HMONITOR>*>(p)->push_back(m);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&list));
    return list;
}

struct WindowSearch {
    std::wstring_view needle;
    HWND found = nullptr;
};

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM p)
{
    auto* s = reinterpret_cast<WindowSearch*>(p);
    if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd)
        return TRUE;
    wchar_t title[512]{};
    const int len = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    if (len > 0 && std::wstring_view(title, static_cast<size_t>(len)).find(s->needle) != std::wstring_view::npos) {
        s->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

winrt::com_ptr<ID3D11Texture2D> TextureFromSurface(wgdx::Direct3D11::IDirect3DSurface const& surface)
{
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));
    return tex;
}

} // namespace

WgcCapture::GraphicsCaptureItem WgcCapture::ItemForMonitor(int index)
{
    const auto monitors = EnumMonitors();
    if (index < 0 || index >= static_cast<int>(monitors.size()))
        throw std::runtime_error(std::format("Монитор {} не найден (всего {})", index, monitors.size()));

    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForMonitor(monitors[static_cast<size_t>(index)],
                                                   winrt::guid_of<wgc::IGraphicsCaptureItem>(),
                                                   winrt::put_abi(item)));
    return item;
}

WgcCapture::GraphicsCaptureItem WgcCapture::ItemForWindow(std::wstring_view titlePart)
{
    WindowSearch search{ titlePart };
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&search));
    if (!search.found)
        throw std::runtime_error("Окно с таким заголовком не найдено");
    return ItemForHwnd(search.found);
}

WgcCapture::GraphicsCaptureItem WgcCapture::ItemForHwnd(HWND hwnd)
{
    if (!IsWindow(hwnd))
        throw std::runtime_error("Окно уже закрыто");
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::IGraphicsCaptureItem>(),
                                                  winrt::put_abi(item)));
    return item;
}

HMONITOR MonitorByIndex(int index)
{
    const auto monitors = EnumMonitors();
    return index >= 0 && index < static_cast<int>(monitors.size()) ? monitors[static_cast<size_t>(index)] : nullptr;
}

std::vector<MonitorEntry> ListMonitors()
{
    std::vector<MonitorEntry> out;
    const auto monitors = EnumMonitors();
    for (size_t i = 0; i < monitors.size(); ++i) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(monitors[i], &mi);
        MonitorEntry e;
        e.index = static_cast<int>(i);
        e.width = mi.rcMonitor.right - mi.rcMonitor.left;
        e.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        e.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        e.name = WideToUtf8(mi.szDevice);
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<WindowEntry> ListWindows(HWND exclude)
{
    struct Ctx {
        HWND exclude;
        std::vector<WindowEntry> out;
    } ctx{ exclude, {} };

    EnumWindows(
        [](HWND hwnd, LPARAM p) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(p);
            if (hwnd == c->exclude || GetAncestor(hwnd, GA_ROOT) != hwnd)
                return TRUE;
            // Свёрнутое окно IsWindowVisible не проходит — его всё равно можно выбрать.
            if (!IsWindowVisible(hwnd) && !IsIconic(hwnd))
                return TRUE;
            if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
                return TRUE;
            DWORD cloaked = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                return TRUE;
            wchar_t title[512]{};
            const int len = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
            if (len <= 0)
                return TRUE;
            c->out.push_back({ hwnd, std::wstring(title, static_cast<size_t>(len)) });
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

WgcCapture::WgcCapture(const D3DContext& d3d, GraphicsCaptureItem item, bool cursor)
    : d3d_(d3d), item_(std::move(item))
{
    poolSize_ = item_.Size();
    width_ = static_cast<uint32_t>(std::max(poolSize_.Width, 2)) & ~1u;
    height_ = static_cast<uint32_t>(std::max(poolSize_.Height, 2)) & ~1u;
    LogInfo("Источник: %s, %ux%u\n", WideToUtf8(item_.DisplayName()).c_str(), width_, height_);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(d3d_.device->CreateTexture2D(&desc, nullptr, latest_.put()), "CreateTexture2D(latest)");
    ThrowIfFailed(d3d_.device->CreateRenderTargetView(latest_.get(), nullptr, latestRtv_.put()),
                  "CreateRenderTargetView(latest)");

    pool_ = FramePool::CreateFreeThreaded(d3d_.winrtDevice, kPixelFormat, kPoolBuffers, poolSize_);
    session_ = pool_.CreateCaptureSession(item_);

    try {
        session_.IsCursorCaptureEnabled(cursor);  // Windows 10 2004+
    } catch (const winrt::hresult_error&) {
    }
    try {
        session_.IsBorderRequired(false);  // Windows 11; на Win10 жёлтая рамка остаётся
    } catch (const winrt::hresult_error&) {
    }

    frameArrived_ = pool_.FrameArrived(winrt::auto_revoke, { this, &WgcCapture::OnFrameArrived });
    itemClosed_ = item_.Closed(winrt::auto_revoke, [this](auto&&, auto&&) { closed_ = true; });
}

WgcCapture::~WgcCapture()
{
    Stop();
}

void WgcCapture::Start()
{
    session_.StartCapture();
}

void WgcCapture::Stop()
{
    frameArrived_.revoke();
    itemClosed_.revoke();
    if (session_) {
        session_.Close();
        session_ = nullptr;
    }
    if (pool_) {
        pool_.Close();
        pool_ = nullptr;
    }
}

void WgcCapture::OnFrameArrived(FramePool const& sender, winrt::Windows::Foundation::IInspectable const&)
{
    const auto frame = sender.TryGetNextFrame();
    if (!frame)
        return;

    const auto contentSize = frame.ContentSize();
    const auto src = TextureFromSurface(frame.Surface());

    D3D11_TEXTURE2D_DESC srcDesc{};
    src->GetDesc(&srcDesc);

    D3D11_BOX box{};
    box.right = std::min({ static_cast<UINT>(std::max(contentSize.Width, 0)), srcDesc.Width, width_ });
    box.bottom = std::min({ static_cast<UINT>(std::max(contentSize.Height, 0)), srcDesc.Height, height_ });
    box.back = 1;

    const bool resized = contentSize.Width != poolSize_.Width || contentSize.Height != poolSize_.Height;
    {
        std::lock_guard lock(mtx_);
        // Размер выхода фиксирован (кодировщик не пересоздаётся): при изменении размера окна
        // остаток кадра заливается чёрным, лишнее обрезается.
        if (resized || box.right < width_ || box.bottom < height_) {
            const float black[4] = { 0.f, 0.f, 0.f, 1.f };
            d3d_.context->ClearRenderTargetView(latestRtv_.get(), black);
        }
        if (box.right > 0 && box.bottom > 0)
            d3d_.context->CopySubresourceRegion(latest_.get(), 0, 0, 0, 0, src.get(), 0, &box);
        hasFrame_ = true;
    }

    if (resized && contentSize.Width > 0 && contentSize.Height > 0) {
        poolSize_ = contentSize;
        sender.Recreate(d3d_.winrtDevice, kPixelFormat, kPoolBuffers, poolSize_);
    }
}

bool WgcCapture::CopyLatest(ID3D11Texture2D* dst)
{
    std::lock_guard lock(mtx_);
    if (!hasFrame_)
        return false;
    d3d_.context->CopyResource(dst, latest_.get());
    return true;
}
