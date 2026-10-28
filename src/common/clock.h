#pragma once

#include <windows.h>

#include <cstdint>

inline int64_t QpcNow()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

inline int64_t QpcFrequency()
{
    static const int64_t freq = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return freq;
}

// a * b / c без переполнения промежуточного произведения (b, c > 0).
inline int64_t MulDiv64(int64_t a, int64_t b, int64_t c)
{
    const int64_t q = a / c;
    const int64_t r = a % c;
    return q * b + r * b / c;
}

// Общие часы A/V: всё приводится к 100 нс от момента t0 (QPC).
// WASAPI отдаёт QPC-позицию пакета уже в 100 нс, поэтому видео и аудио сравнимы напрямую.
class MediaClock {
public:
    explicit MediaClock(int64_t t0Qpc)
        : t0Qpc_(t0Qpc), t0Hns_(MulDiv64(t0Qpc, 10'000'000, QpcFrequency())) {}

    int64_t PtsFromQpc(int64_t qpc) const { return MulDiv64(qpc - t0Qpc_, 10'000'000, QpcFrequency()); }
    int64_t PtsFromQpcHns(int64_t qpcHns) const { return qpcHns - t0Hns_; }
    int64_t NowPts() const { return PtsFromQpc(QpcNow()); }

private:
    int64_t t0Qpc_;
    int64_t t0Hns_;
};
