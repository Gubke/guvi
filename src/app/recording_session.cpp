#include "app/recording_session.h"
#include "common/hr.h"
#include "common/log.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::wstring SidecarFor(const std::wstring& mp4) { return mp4 + L".guvi"; }

std::wstring Utf8ToWide(std::string_view s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

void WriteSidecar(const std::wstring& mp4)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const auto name = std::filesystem::path(mp4).stem().wstring();
    const std::string body = "path=" + WideToUtf8(mp4) + "\nstarted=" +
                             std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour,
                                         t.wMinute, t.wSecond) +
                             "\nname=" + WideToUtf8(name) + "\n";
    const std::wstring side = SidecarFor(mp4);
    HANDLE file = CreateFileW(side.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

void DeleteSidecar(const std::wstring& mp4) { DeleteFileW(SidecarFor(mp4).c_str()); }

bool EndsWith(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + (s.size() - n), suffix) == 0;
}

bool InsideDir(const std::filesystem::path& root, const std::filesystem::path& file)
{
    std::error_code ec;
    const auto base = std::filesystem::weakly_canonical(root, ec);
    if (ec)
        return false;
    const auto full = std::filesystem::weakly_canonical(file, ec);
    if (ec)
        return false;
    auto rel = std::filesystem::relative(full, base, ec);
    if (ec)
        return false;
    const auto native = rel.native();
    return !native.empty() && native != L".." && !native.starts_with(L"..\\") && !native.starts_with(L"../");
}

uint32_t Be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint64_t Be64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}

bool SameType(const uint8_t* p, char a, char b, char c, char d)
{
    return p[0] == static_cast<uint8_t>(a) && p[1] == static_cast<uint8_t>(b) && p[2] == static_cast<uint8_t>(c) &&
           p[3] == static_cast<uint8_t>(d);
}

// Обрезать оборванный хвост ISO BMFF. Висящий moof без mdat снимается.
// Уже целый файл не укорачивается. Сам ролик не удаляется.
bool RepairFragmentedMp4(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 8) {
        CloseHandle(file);
        return size.QuadPart > 0;
    }
    const uint64_t fileSize = static_cast<uint64_t>(size.QuadPart);

    struct Box {
        uint64_t end = 0;
        char type[4]{};
    };
    std::vector<Box> boxes;
    uint64_t pos = 0;
    while (pos + 8 <= fileSize) {
        uint8_t hdr[16]{};
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(pos);
        if (!SetFilePointerEx(file, li, nullptr, FILE_BEGIN))
            break;
        DWORD got = 0;
        if (!ReadFile(file, hdr, 8, &got, nullptr) || got != 8)
            break;
        const uint32_t size32 = Be32(hdr);
        uint64_t boxSize = 0;
        if (size32 == 1) {
            if (pos + 16 > fileSize || !ReadFile(file, hdr + 8, 8, &got, nullptr) || got != 8)
                break;
            boxSize = Be64(hdr + 8);
            if (boxSize < 16)
                break;
        } else if (size32 == 0) {
            break;
        } else {
            boxSize = size32;
            if (boxSize < 8)
                break;
        }
        if (pos + boxSize > fileSize)
            break;
        Box box;
        box.end = pos + boxSize;
        std::memcpy(box.type, hdr + 4, 4);
        boxes.push_back(box);
        pos = box.end;
    }

    while (!boxes.empty() && SameType(reinterpret_cast<const uint8_t*>(boxes.back().type), 'm', 'o', 'o', 'f'))
        boxes.pop_back();

    bool ok = !boxes.empty();
    if (!boxes.empty() && boxes.back().end < fileSize) {
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(boxes.back().end);
        if (SetFilePointerEx(file, li, nullptr, FILE_BEGIN) && SetEndOfFile(file))
            ok = true;
        else
            ok = false;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

std::wstring ValueOf(const std::string& text, const char* key)
{
    const std::string prefix = std::string(key) + "=";
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with(prefix))
            return Utf8ToWide(std::string_view(line).substr(prefix.size()));
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return {};
}

} // namespace

RecordingSession::RecordingSession(std::wstring path, const StreamSetup& setup, int64_t startPts)
    : path_(std::move(path)), startPts_(startPts)
{
    const bool mp4 = EndsWith(path_, L".mp4");
    if (mp4)
        WriteSidecar(path_);
    try {
        muxer_ = CreateMuxer(path_, setup, startPts_, mp4);
    } catch (...) {
        std::error_code ec;
        const auto sz = std::filesystem::exists(path_, ec) ? std::filesystem::file_size(path_, ec) : 0;
        if (ec || sz == 0) {
            std::filesystem::remove(path_, ec);
            DeleteSidecar(path_);
        }
        throw;
    }
    waitAudio_ = muxer_->SupportsAudio();
    if (setup.audio && !waitAudio_)
        LogInfo("Внимание: в сырой поток аудио не пишется\n");
    LogInfo("● Запись: %s\n", WideToUtf8(path_).c_str());
}

void RecordingSession::OnPacket(const Packet& p)
{
    if (p.pts < startPts_ || p.pts >= stopPts_) {
        if (p.stream == StreamKind::Audio && p.pts >= stopPts_)
            audioDone_ = true;
        return;
    }
    // Кодирование идёт в очереди: кадры до stopPts_ могут прийти уже после RequestStop — их тоже пишем.
    if (p.stream == StreamKind::Video)
        lastVideoPts_ = p.pts;
    muxer_->Write(p);
    bytes_ += p.data.size();
}

void RecordingSession::RequestStop(int64_t stopPts, int64_t deadlineQpc)
{
    if (stopping_)
        return;
    stopping_ = true;
    stopPts_ = stopPts;
    deadlineQpc_ = deadlineQpc;
}

bool RecordingSession::ReadyToFinish(int64_t nowQpc) const
{
    return stopping_ && (!waitAudio_ || audioDone_ || nowQpc >= deadlineQpc_);
}

void RecordingSession::Finish()
{
    if (!muxer_)
        return;
    muxer_->Finish();
    muxer_.reset();
    DeleteSidecar(path_);
    const double seconds = lastVideoPts_ >= startPts_
                               ? static_cast<double>(lastVideoPts_ - startPts_) / kTimescale
                               : 0.0;
    LogInfo("■ Запись сохранена: %s (%.1f с, %.1f МБ)\n", WideToUtf8(path_).c_str(), seconds,
                static_cast<double>(bytes_) / (1024.0 * 1024.0));
}

std::vector<std::wstring> RecoverUnfinishedRecordings(const std::wstring& directory)
{
    std::vector<std::wstring> restored;
    std::error_code ec;
    if (directory.empty() || !std::filesystem::is_directory(directory, ec))
        return restored;

    for (const auto& ent : std::filesystem::directory_iterator(directory, ec)) {
        if (ec || !ent.is_regular_file())
            continue;
        const auto sideName = ent.path().filename().wstring();
        if (!EndsWith(sideName, L".mp4.guvi"))
            continue;

        std::string text;
        {
            std::ifstream in(ent.path(), std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::wstring started = ValueOf(text, "started");
        const std::wstring name = ValueOf(text, "name");
        std::wstring media = ent.path().wstring();
        constexpr std::wstring_view kSuffix = L".guvi";
        if (media.size() > kSuffix.size())
            media.erase(media.size() - kSuffix.size());

        const std::wstring listed = ValueOf(text, "path");
        std::error_code existsEc;
        if (!std::filesystem::is_regular_file(media, existsEc) && !listed.empty() && InsideDir(directory, listed))
            media = listed;
        if (!InsideDir(directory, media) || !std::filesystem::is_regular_file(media, existsEc)) {
            DeleteFileW(ent.path().c_str());
            continue;
        }

        HANDLE probe = CreateFileW(media.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE)
            continue;
        LARGE_INTEGER probed{};
        const BOOL sized = GetFileSizeEx(probe, &probed);
        CloseHandle(probe);
        if (!sized || probed.QuadPart < 8) {
            DeleteFileW(ent.path().c_str());
            continue;
        }

        std::wstring finalPath = media;
        if (!name.empty()) {
            const auto desired = std::filesystem::path(directory) / (name + L".mp4");
            if (_wcsicmp(desired.c_str(), media.c_str()) != 0 && !std::filesystem::exists(desired, existsEc)) {
                std::error_code moveEc;
                std::filesystem::rename(media, desired, moveEc);
                if (!moveEc)
                    finalPath = desired.wstring();
            }
        }

        if (!RepairFragmentedMp4(finalPath))
            continue;

        std::error_code sizeEc;
        const auto bytes = std::filesystem::file_size(finalPath, sizeEc);
        DeleteFileW(ent.path().c_str());
        if (sizeEc || bytes < 8)
            continue;
        LogInfo("Восстановлено видео: %s%s%s", WideToUtf8(finalPath).c_str(), started.empty() ? "" : " (начато ",
                started.empty() ? "" : (WideToUtf8(started) + ")").c_str());
        restored.push_back(finalPath);
    }
    return restored;
}
