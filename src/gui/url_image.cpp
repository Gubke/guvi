#include "gui/url_image.h"

#include <objidl.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <wincodec.h>
#include <unknwn.h>
#include <winrt/base.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace {

constexpr size_t kMaxBytes = 2 * 1024 * 1024;
constexpr UINT kMaxEdge = 1280;

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

void GdiplusOnce()
{
    static ULONG_PTR token = 0;
    static std::once_flag once;
    std::call_once(once, [] {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&token, &in, nullptr);
    });
}

UrlBitmap MakeCard(const std::string& url, bool failed)
{
    GdiplusOnce();
    constexpr int w = 960;
    constexpr int h = 180;
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::SolidBrush bg(Gdiplus::Color(235, 18, 18, 22));
    g.FillRectangle(&bg, 0, 0, w, h);
    Gdiplus::SolidBrush accent(Gdiplus::Color(255, 227, 38, 54));
    g.FillRectangle(&accent, 0, 0, 8, h);

    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font title(&family, 28.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font body(&family, 32.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 240, 240, 244));
    Gdiplus::SolidBrush muted(Gdiplus::Color(255, 170, 172, 180));
    const wchar_t* cap = failed ? L"ссылка не открылась" : L"ссылка";
    g.DrawString(cap, -1, &title, Gdiplus::PointF(28, 28), &muted);

    std::wstring text = Utf8ToWide(url);
    if (text.size() > 120)
        text.resize(120);
    Gdiplus::RectF box(28, 78, w - 56.0f, 80);
    g.DrawString(text.c_str(), -1, &body, box, nullptr, &white);

    Gdiplus::BitmapData bits{};
    Gdiplus::Rect rect(0, 0, w, h);
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bits) != Gdiplus::Ok)
        return {};
    auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const auto* src = static_cast<const uint8_t*>(bits.Scan0) + static_cast<size_t>(y) * bits.Stride;
        std::memcpy(pixels->data() + static_cast<size_t>(y) * w * 4, src, static_cast<size_t>(w) * 4);
    }
    bmp.UnlockBits(&bits);

    UrlBitmap out;
    out.url = url;
    out.bgra = std::move(pixels);
    out.width = w;
    out.height = h;
    out.failed = failed;
    return out;
}

bool LooksLikeImage(const uint8_t* p, size_t n, const std::wstring& contentType)
{
    if (contentType.find(L"image/") != std::wstring::npos)
        return true;
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G')
        return true;
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8)
        return true;
    if (n >= 6 && (std::memcmp(p, "GIF87a", 6) == 0 || std::memcmp(p, "GIF89a", 6) == 0))
        return true;
    if (n >= 2 && p[0] == 'B' && p[1] == 'M')
        return true;
    if (n >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0)
        return true;
    return false;
}

struct HttpHandle {
    HINTERNET h = nullptr;
    explicit HttpHandle(HINTERNET p = nullptr) : h(p) {}
    ~HttpHandle()
    {
        if (h)
            WinHttpCloseHandle(h);
    }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

bool Download(const std::wstring& url, std::vector<uint8_t>& body, std::wstring& contentType)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    wchar_t extra[1024]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        return false;
    const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (parts.nScheme != INTERNET_SCHEME_HTTP && !https)
        return false;

    HttpHandle session(WinHttpOpen(L"GuVi", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        return false;
    WinHttpSetTimeouts(session.h, 4000, 4000, 8000, 8000);
    HttpHandle connect(WinHttpConnect(session.h, host, parts.nPort, 0));
    if (!connect)
        return false;
    const DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    std::wstring object = path[0] ? path : L"/";
    object += extra;
    HttpHandle req(WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req)
        return false;
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return false;
    if (!WinHttpReceiveResponse(req.h, nullptr))
        return false;

    DWORD status = 0;
    DWORD sz = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
        return false;

    wchar_t ct[160]{};
    DWORD ctSize = sizeof(ct);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct, &ctSize,
                            WINHTTP_NO_HEADER_INDEX))
        contentType = ct;

    body.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail))
            return false;
        if (avail == 0)
            break;
        if (body.size() + avail > kMaxBytes)
            return false;
        const size_t off = body.size();
        body.resize(off + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, body.data() + off, avail, &read))
            return false;
        body.resize(off + read);
    }
    return !body.empty();
}

UrlBitmap DecodeImage(const std::string& url, const std::vector<uint8_t>& body)
{
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))))
        return {};
    winrt::com_ptr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put())))
        return {};
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(body.data()), static_cast<DWORD>(body.size()))))
        return {};
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())))
        return {};
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put())))
        return {};
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0)
        return {};

    winrt::com_ptr<IWICBitmapSource> source;
    source.copy_from(frame.get());
    if (w > kMaxEdge || h > kMaxEdge) {
        const double scale = std::min(static_cast<double>(kMaxEdge) / w, static_cast<double>(kMaxEdge) / h);
        w = std::max(1u, static_cast<UINT>(w * scale));
        h = std::max(1u, static_cast<UINT>(h * scale));
        winrt::com_ptr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(scaler.put())))
            return {};
        if (FAILED(scaler->Initialize(frame.get(), w, h, WICBitmapInterpolationModeFant)))
            return {};
        source = scaler;
    }

    winrt::com_ptr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(conv.put())))
        return {};
    if (FAILED(conv->Initialize(source.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return {};
    auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels->size()), pixels->data())))
        return {};

    UrlBitmap out;
    out.url = url;
    out.bgra = std::move(pixels);
    out.width = w;
    out.height = h;
    return out;
}

UrlBitmap LoadUrl(const std::string& url)
{
    const std::wstring wide = Utf8ToWide(url);
    std::vector<uint8_t> body;
    std::wstring contentType;
    if (!Download(wide, body, contentType) || !LooksLikeImage(body.data(), body.size(), contentType))
        return MakeCard(url, true);
    UrlBitmap image = DecodeImage(url, body);
    if (!image.bgra)
        return MakeCard(url, true);
    return image;
}

} // namespace

struct UrlImageCache::Impl {
    struct SlotState {
        std::string url;
        uint64_t token = 0;
        UrlBitmap bitmap;
    };

    Impl()
    {
        thread_ = std::thread([this] { Thread(); });
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    UrlBitmap Slot(int index, const std::string& url)
    {
        std::lock_guard lock(mtx_);
        SlotState& s = slots_[index];
        if (s.url == url)
            return s.bitmap;
        s.url = url;
        ++s.token;
        s.bitmap = {};
        s.bitmap.url = url;
        if (url.empty())
            return s.bitmap;
        s.bitmap.pending = true;
        queue_.push_back({ index, url, s.token });
        cv_.notify_one();
        return s.bitmap;
    }

    void Thread()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mtx_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty())
                    break;
                job = queue_.front();
                queue_.pop_front();
                if (slots_[job.slot].token != job.token)
                    continue;
            }
            UrlBitmap loaded = LoadUrl(job.url);
            std::lock_guard lock(mtx_);
            SlotState& s = slots_[job.slot];
            if (s.token != job.token || s.url != job.url)
                continue;
            loaded.seq = ++seq_;
            loaded.pending = false;
            s.bitmap = std::move(loaded);
        }
        CoUninitialize();
    }

    struct Job {
        int slot = 0;
        std::string url;
        uint64_t token = 0;
    };

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    SlotState slots_[kSlots]{};
    uint64_t seq_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

UrlImageCache::UrlImageCache() : impl_(std::make_unique<Impl>()) {}

UrlImageCache::~UrlImageCache() = default;

UrlBitmap UrlImageCache::Slot(int index, const std::string& url)
{
    if (index < 0 || index >= kSlots)
        return {};
    return impl_->Slot(index, url);
}
