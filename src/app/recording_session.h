#pragma once

#include "common/types.h"
#include "mux/muxer.h"

#include <memory>
#include <string>
#include <vector>

// Одна запись в файл: [startPts, stopPts). startPts — PTS принудительного IDR.
// После RequestStop() видео больше не пишется, а аудио дописывается, пока не придёт
// пакет с PTS >= stopPts (аудио отстаёт от видео на задержку микшера) или не выйдет таймаут.
class RecordingSession {
public:
    RecordingSession(std::wstring path, const StreamSetup& setup, int64_t startPts);

    void OnPacket(const Packet& p);
    void RequestStop(int64_t stopPts, int64_t deadlineQpc);

    bool Stopping() const { return stopping_; }
    bool ReadyToFinish(int64_t nowQpc) const;
    void Finish();

    int64_t StartPts() const { return startPts_; }
    const std::wstring& Path() const { return path_; }

private:
    std::wstring path_;
    std::unique_ptr<IMuxer> muxer_;
    int64_t startPts_;
    int64_t stopPts_ = INT64_MAX;
    int64_t deadlineQpc_ = 0;
    int64_t lastVideoPts_ = -1;
    bool stopping_ = false;
    bool audioDone_ = false;
    bool waitAudio_ = false;
    uint64_t bytes_ = 0;
};

// Незаконченная запись — файл .mp4 рядом с sidecar «.mp4.guvi» (path, started, name).
// Чинит фрагменты на месте, оставляет ролик под обычным именем и не удаляет его.
// Возвращает полные пути восстановленных файлов.
std::vector<std::wstring> RecoverUnfinishedRecordings(const std::wstring& directory);
