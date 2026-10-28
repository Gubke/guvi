#pragma once

#include "common/d3d_device.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct MonitorEntry {
    int index = 0;
    std::string name;  // \\.\DISPLAYn
    int width = 0;
    int height = 0;
    bool primary = false;
};

struct WindowEntry {
    HWND hwnd = nullptr;
    std::wstring title;
};

// Порядок мониторов совпадает с индексом для WgcCapture::ItemForMonitor.
std::vector<MonitorEntry> ListMonitors();
HMONITOR MonitorByIndex(int index);  // nullptr — нет такого
// Окна верхнего уровня с заголовком, включая свёрнутые (без tool-окон и скрытых UWP).
std::vector<WindowEntry> ListWindows(HWND exclude = nullptr);

// Модуль 1: Windows.Graphics.Capture → D3D11-текстура, без чтения в CPU.
// Кадры приходят в пул потоков (free-threaded frame pool); последний кадр копируется
// в собственную текстуру фиксированного размера, откуда его забирает кодировщик.
class WgcCapture {
public:
    using GraphicsCaptureItem = winrt::Windows::Graphics::Capture::GraphicsCaptureItem;

    static GraphicsCaptureItem ItemForMonitor(int index);
    static GraphicsCaptureItem ItemForWindow(std::wstring_view titlePart);
    static GraphicsCaptureItem ItemForHwnd(HWND hwnd);

    WgcCapture(const D3DContext& d3d, GraphicsCaptureItem item, bool cursor);
    ~WgcCapture();

    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    void Start();
    void Stop();

    // Размер выходной текстуры (размер источника на момент старта, округлённый до чётного).
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

    // Копирует последний кадр в dst (B8G8R8A8, Width x Height). false — кадров ещё не было.
    bool CopyLatest(ID3D11Texture2D* dst);

    bool Closed() const { return closed_.load(); }

private:
    using FramePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;

    void OnFrameArrived(FramePool const& sender, winrt::Windows::Foundation::IInspectable const&);

    D3DContext d3d_;
    GraphicsCaptureItem item_{ nullptr };
    FramePool pool_{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{ nullptr };
    FramePool::FrameArrived_revoker frameArrived_;
    GraphicsCaptureItem::Closed_revoker itemClosed_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    winrt::Windows::Graphics::SizeInt32 poolSize_{};

    std::mutex mtx_;
    winrt::com_ptr<ID3D11Texture2D> latest_;
    winrt::com_ptr<ID3D11RenderTargetView> latestRtv_;
    bool hasFrame_ = false;
    std::atomic<bool> closed_{ false };
};
