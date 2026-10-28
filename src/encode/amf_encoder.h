#pragma once

#include "encode/video_encoder.h"

#include <memory>

// AMD AMF (amfrt64.dll из драйвера Adrenalin, грузится динамически). Вход — DX11-текстура
// BGRA, конвертацию в NV12 делает сам VCN. Usage LOW_LATENCY, CBR, без B-кадров.
std::unique_ptr<IVideoEncoder> CreateAmfEncoder(ID3D11Device* device, const VideoStreamInfo& info);
