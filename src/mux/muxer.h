#pragma once

#include "common/types.h"

#include <memory>
#include <string>

class IMuxer {
public:
    virtual ~IMuxer() = default;
    // Пакеты одного потока — в порядке PTS; PTS < timeOffset не допускаются.
    virtual void Write(const Packet& pkt) = 0;
    virtual void Finish() = 0;
    virtual bool SupportsAudio() const = 0;
};

// .mp4 — Media Foundation Sink Writer (passthrough готовых H.264/HEVC и AAC сэмплов).
// .h264/.264/.hevc/.h265/.265 — сырой Annex B поток, аудио отбрасывается.
// timeOffset вычитается из PTS: файл начинается с 0 (первый видеокадр — IDR с PTS = timeOffset).
// crashSafe для .mp4 — фрагментированный файл: готовые куски уже на диске, обрыв питания
// не уничтожает весь дубль. Короткий повтор (replay) пишется обычным MP4.
std::unique_ptr<IMuxer> CreateMuxer(const std::wstring& path, const StreamSetup& setup, int64_t timeOffset,
                                    bool crashSafe = false);
