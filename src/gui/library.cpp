#include "gui/library.h"
#include "common/hr.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>

namespace {

constexpr SIZE kThumbSize{ 320, 180 };
constexpr uint64_t kTicksPerSecond = 10'000'000;  // FILETIME

uint64_t NowFiletime()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool IsVideo(const std::filesystem::path& p)
{
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext == L".mp4" || ext == L".h264" || ext == L".264" || ext == L".hevc" || ext == L".h265" || ext == L".265";
}

std::string DateText(uint64_t ft)
{
    FILETIME utc{ static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32) };
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st))
        return {};
    return std::format("{:02}.{:02}.{:04} {:02}:{:02}", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
}

bool LoadThumbnail(const std::wstring& path, uint32_t& w, uint32_t& h, std::vector<uint8_t>& bgra)
{
    winrt::com_ptr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(factory.put()))))
        return false;
    HBITMAP bmp = nullptr;
    // THUMBNAILONLY: без иконки-заглушки, если у файла нет кадра (тогда рисуем свою).
    if (FAILED(factory->GetImage(kThumbSize, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &bmp)) || !bmp)
        return false;

    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    bool ok = false;
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        w = static_cast<uint32_t>(bm.bmWidth);
        h = static_cast<uint32_t>(bm.bmHeight);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bgra.resize(static_cast<size_t>(w) * h * 4);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, bmp, 0, h, bgra.data(), &bi, DIB_RGB_COLORS) == static_cast<int>(h);
        ReleaseDC(nullptr, dc);
        // Альфа у кадров видео бывает нулевой — миниатюра должна быть непрозрачной.
        for (size_t i = 3; i < bgra.size(); i += 4)
            bgra[i] = 0xFF;
    }
    DeleteObject(bmp);
    return ok;
}

} // namespace

RecordingLibrary::RecordingLibrary()
{
    worker_ = std::thread(&RecordingLibrary::Worker, this);
}

RecordingLibrary::~RecordingLibrary()
{
    {
        std::lock_guard lock(mtx_);
        quit_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

void RecordingLibrary::Worker()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mtx_);
            cv_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
            if (quit_)
                break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Done d;
        d.path = job.path;
        d.mtime = job.mtime;
        if (!LoadThumbnail(job.path, d.w, d.h, d.bgra))
            d.bgra.clear();
        std::lock_guard lock(mtx_);
        done_.push_back(std::move(d));
    }
    CoUninitialize();
}

void RecordingLibrary::Scan()
{
    items_.clear();
    sortedBy_ = -1;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(folder_, ec)) {
        if (!e.is_regular_file(ec) || !IsVideo(e.path()))
            continue;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(e.path().c_str(), GetFileExInfoStandard, &fa))
            continue;
        RecordingItem it;
        it.path = e.path().wstring();
        it.name = WideToUtf8(e.path().filename().wstring());
        it.size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        it.mtime = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
        it.dateText = DateText(it.mtime);
        items_.push_back(std::move(it));
    }

    ULARGE_INTEGER freeBytes{}, totalBytes{};
    if (GetDiskFreeSpaceExW(folder_.c_str(), &freeBytes, &totalBytes, nullptr)) {
        free_ = freeBytes.QuadPart;
        total_ = totalBytes.QuadPart;
    } else {
        // Папки ещё нет — смотрим её диск.
        const auto root = std::filesystem::path(folder_).root_path().wstring();
        if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &totalBytes, nullptr)) {
            free_ = freeBytes.QuadPart;
            total_ = totalBytes.QuadPart;
        } else {
            free_ = total_ = 0;
        }
    }
}

void RecordingLibrary::Tick(ID3D11Device* device, const std::wstring& folder, const std::wstring& busyPath)
{
    busy_ = busyPath;
    const uint64_t now = NowFiletime();
    if (folder != folder_) {
        folder_ = folder;
        thumbs_.clear();
        nextScan_ = 0;
        std::lock_guard lock(mtx_);
        jobs_.clear();
    }
    if (now >= nextScan_) {
        Scan();
        nextScan_ = now + 2 * kTicksPerSecond;
    }

    std::vector<Done> done;
    {
        std::lock_guard lock(mtx_);
        done.swap(done_);
    }
    for (auto& d : done) {
        auto it = thumbs_.find(d.path);
        if (it == thumbs_.end() || it->second.mtime != d.mtime)
            continue;  // файл с тех пор изменился — придёт новый запрос
        Thumb& t = it->second;
        if (d.bgra.empty()) {
            t.failed = true;
            continue;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = d.w;
        desc.Height = d.h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ d.bgra.data(), d.w * 4, 0 };
        winrt::com_ptr<ID3D11Texture2D> tex;
        if (FAILED(device->CreateTexture2D(&desc, &init, tex.put())) ||
            FAILED(device->CreateShaderResourceView(tex.get(), nullptr, t.srv.put()))) {
            t.failed = true;
            continue;
        }
        t.w = d.w;
        t.h = d.h;
    }
}

const std::vector<RecordingItem>& RecordingLibrary::Items(int sort)
{
    if (sort != sortedBy_) {
        std::sort(items_.begin(), items_.end(), [sort](const RecordingItem& a, const RecordingItem& b) {
            switch (sort) {
            case Oldest: return a.mtime < b.mtime;
            case Name: return a.name < b.name;
            case Size: return a.size > b.size;
            default: return a.mtime > b.mtime;
            }
        });
        sortedBy_ = sort;
    }
    return items_;
}

ID3D11ShaderResourceView* RecordingLibrary::Thumbnail(const RecordingItem& item, uint32_t* w, uint32_t* h)
{
    Thumb& t = thumbs_[item.path];
    if (t.mtime != item.mtime) {
        t = Thumb{};
        t.mtime = item.mtime;
    }
    // Файл ещё пишется или только что изменён — кадра для миниатюры может не быть.
    const bool fresh = NowFiletime() < item.mtime + 3 * kTicksPerSecond;
    if (!t.requested && !t.failed && item.path != busy_ && !fresh) {
        t.requested = true;
        {
            std::lock_guard lock(mtx_);
            jobs_.push_back({ item.path, item.mtime });
        }
        cv_.notify_one();
    }
    if (!t.srv)
        return nullptr;
    *w = t.w;
    *h = t.h;
    return t.srv.get();
}
