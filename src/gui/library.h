#pragma once

#include <unknwn.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct RecordingItem {
    std::wstring path;
    std::string name;      // UTF-8, имя файла
    std::string dateText;  // "28.10.2026 18:20"
    uint64_t size = 0;
    uint64_t mtime = 0;    // FILETIME
};

// Левая панель GUI: видео в папке записей, их миниатюры и свободное место на диске.
// Миниатюры берёт оболочка Windows (IShellItemImageFactory — тот же кэш, что у Проводника)
// в фоновом STA-потоке; в текстуры UI-устройства они загружаются в потоке окна.
class RecordingLibrary {
public:
    enum Sort { Newest = 0, Oldest = 1, Name = 2, Size = 3 };

    RecordingLibrary();
    ~RecordingLibrary();

    RecordingLibrary(const RecordingLibrary&) = delete;
    RecordingLibrary& operator=(const RecordingLibrary&) = delete;

    // Раз в кадр из потока UI: пересканирует папку (не чаще раза в 2 с) и грузит готовые миниатюры.
    // busyPath — файл, который сейчас пишется (его миниатюра не запрашивается).
    void Tick(ID3D11Device* device, const std::wstring& folder, const std::wstring& busyPath);
    void RescanNow() { nextScan_ = 0; }

    const std::vector<RecordingItem>& Items(int sort);
    // nullptr — ещё не готова или недоступна; заодно ставит в очередь.
    ID3D11ShaderResourceView* Thumbnail(const RecordingItem& item, uint32_t* w, uint32_t* h);

    uint64_t FreeBytes() const { return free_; }
    uint64_t TotalBytes() const { return total_; }
    const std::wstring& Folder() const { return folder_; }

private:
    struct Thumb {
        uint64_t mtime = 0;
        bool requested = false;
        bool failed = false;
        uint32_t w = 0, h = 0;
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
    };
    struct Job {
        std::wstring path;
        uint64_t mtime = 0;
    };
    struct Done {
        std::wstring path;
        uint64_t mtime = 0;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> bgra;  // пусто — не получилось
    };

    void Scan();
    void Worker();

    std::wstring folder_;
    std::wstring busy_;
    std::vector<RecordingItem> items_;
    int sortedBy_ = -1;
    uint64_t nextScan_ = 0;
    uint64_t free_ = 0, total_ = 0;
    std::map<std::wstring, Thumb> thumbs_;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<Done> done_;
    bool quit_ = false;
    std::thread worker_;
};
