#pragma once

#include "encode/video_encoder.h"

#include <unknwn.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <nvEncodeAPI.h>

#include <vector>

// Модуль 2: NVENC через NVENC API (nvEncodeAPI64.dll грузится динамически).
// Вход — D3D11-текстура B8G8R8A8 (NV_ENC_BUFFER_FORMAT_ARGB), без копий через CPU.
// Синхронный режим, без B-кадров, preset P4 + tuning LOW_LATENCY, CBR.
class NvencEncoder final : public IVideoEncoder {
public:
    NvencEncoder(ID3D11Device* device, const VideoStreamInfo& info);
    ~NvencEncoder() override;

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    ID3D11Texture2D* InputTexture() const override { return input_.get(); }
    std::vector<uint8_t> SequenceHeader() override;
    void Encode(int64_t pts, bool forceIdr, const PacketSink& sink) override;
    void Flush() override;
    std::string Name() const override { return "NVENC"; }

private:
    void Init(ID3D11Device* device);
    void Release() noexcept;
    void DrainBitstream(const PacketSink& sink);

    VideoStreamInfo info_;
    HMODULE lib_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* encoder_ = nullptr;

    winrt::com_ptr<ID3D11Texture2D> input_;
    NV_ENC_REGISTERED_PTR registered_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
};
