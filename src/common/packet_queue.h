#pragma once

#include "common/types.h"

#include <mutex>
#include <vector>

// Передача пакетов из аудиопотока в главный поток (где живут запись и replay-буфер).
class PacketQueue {
public:
    void Push(PacketPtr p)
    {
        std::lock_guard lock(mtx_);
        items_.push_back(std::move(p));
    }

    void TakeAll(std::vector<PacketPtr>& out)
    {
        std::lock_guard lock(mtx_);
        out.swap(items_);
        items_.clear();
    }

private:
    std::mutex mtx_;
    std::vector<PacketPtr> items_;
};
