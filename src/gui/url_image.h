#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Результат загрузки виджета: картинка по URL или лёгкая карточка с текстом ссылки.
struct UrlBitmap {
    std::string url;
    std::shared_ptr<const std::vector<uint8_t>> bgra;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t seq = 0;
    bool pending = false;
    bool failed = false;
};

// До четырёх слотов. HTTP идёт в фоне, кадр захвата это не ждёт.
class UrlImageCache {
public:
    static constexpr int kSlots = 4;

    UrlImageCache();
    ~UrlImageCache();

    UrlImageCache(const UrlImageCache&) = delete;
    UrlImageCache& operator=(const UrlImageCache&) = delete;

    // Пустой url очищает слот. Повторный вызов с тем же url ничего не качает заново.
    UrlBitmap Slot(int index, const std::string& url);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
