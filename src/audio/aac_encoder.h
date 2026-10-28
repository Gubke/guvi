#pragma once

#include "common/types.h"

#include <unknwn.h>
#include <mftransform.h>
#include <winrt/base.h>

#include <functional>

// AAC-LC через MFT Media Foundation (AAC Encoder). Вход — PCM s16 стерео 48 кГц,
// выход — сырые AAC-кадры по 1024 сэмпла (MF_MT_AAC_PAYLOAD_TYPE = 0).
class AacEncoder {
public:
    static constexpr uint32_t kFrameSamples = 1024;

    using PacketFn = std::function<void(PacketPtr)>;

    // bitrateKbps: 96 / 128 / 160 / 192 (единственные значения, которые принимает MS AAC Encoder).
    AacEncoder(uint32_t sampleRate, uint32_t channels, uint32_t bitrateKbps);

    const AudioStreamInfo& Info() const { return info_; }

    // pcm — interleaved int16, frames кадров; pts — время первого кадра (100 нс).
    void Encode(const int16_t* pcm, uint32_t frames, int64_t pts, const PacketFn& out);
    void Drain(const PacketFn& out);

private:
    void PullOutput(const PacketFn& out);

    AudioStreamInfo info_;
    winrt::com_ptr<IMFTransform> mft_;
    bool providesSamples_ = false;
    DWORD outputSize_ = 0;
    winrt::com_ptr<IMFSample> outSample_;
    winrt::com_ptr<IMFMediaBuffer> outBuffer_;
    int64_t nextPts_ = -1;
    int64_t frameDuration_ = 0;
};
