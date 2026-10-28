#pragma once

#include "encode/video_encoder.h"

#include <memory>

// Аппаратный MFT-кодировщик Media Foundation (Intel Quick Sync; запасной вариант и для AMD/NVIDIA).
// Асинхронный MFT на том же D3D11-устройстве (IMFDXGIDeviceManager), BGRA → NV12 на GPU
// через ID3D11VideoProcessor, без чтения в CPU.
// requireVendorId: 0 — любой аппаратный MFT, иначе только указанного производителя (0x8086 и т.п.).
std::unique_ptr<IVideoEncoder> CreateMfEncoder(const D3DContext& d3d, const VideoStreamInfo& info,
                                               uint32_t requireVendorId);
