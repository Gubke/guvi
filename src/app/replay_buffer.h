#pragma once

#include "common/types.h"

#include <deque>
#include <vector>

// Модуль 4: кольцевой буфер последних N секунд закодированных пакетов в RAM.
// Видео всегда начинается с IDR: обрезка идёт целыми GOP, поэтому в буфере лежит
// от N до N + keyint секунд.
class ReplayBuffer {
public:
    explicit ReplayBuffer(int64_t window) : window_(window) {}

    void Push(const PacketPtr& p);

    // Пакеты (видео+аудио, по возрастанию PTS) от последнего IDR, у которого PTS <= cutoff - window,
    // до cutoff (не включительно). Пусто, если видео ещё нет.
    std::vector<PacketPtr> Snapshot(int64_t cutoff) const;

    size_t Bytes() const { return bytes_; }
    int64_t Window() const { return window_; }

private:
    void Trim();

    int64_t window_;
    std::deque<PacketPtr> video_;
    std::deque<PacketPtr> audio_;
    size_t bytes_ = 0;
};
