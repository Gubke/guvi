#pragma once

#include "common/d3d_device.h"
#include "common/types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class EncoderPref { Auto, Nvenc, Amf, Qsv };

// Общий интерфейс аппаратного кодировщика. Вход — D3D11-текстура B8G8R8A8 размера видео,
// кадр копируется в InputTexture() перед Encode(). Выход — Annex B.
class IVideoEncoder {
public:
    using PacketSink = std::function<void(const EncodedPacket&)>;

    virtual ~IVideoEncoder() = default;

    virtual ID3D11Texture2D* InputTexture() const = 0;
    // SPS/PPS (+VPS для HEVC) в Annex B; может потребовать пробного кадра.
    virtual std::vector<uint8_t> SequenceHeader() = 0;
    virtual void Encode(int64_t pts, bool forceIdr, const PacketSink& sink) = 0;
    virtual void Flush() = 0;

    // Короткое имя бэкенда для UI: "NVENC", "AMD AMF", "Intel Quick Sync".
    virtual std::string Name() const = 0;
};

// Порядок для Auto: по производителю видеокарты устройства (NVIDIA → NVENC, AMD → AMF,
// Intel → Quick Sync), затем запасные варианты. Бросает, если не открылся ни один.
std::unique_ptr<IVideoEncoder> CreateVideoEncoder(const D3DContext& d3d, const VideoStreamInfo& info,
                                                  EncoderPref pref);

// Вытаскивает параметры последовательности (SPS/PPS/VPS) из Annex B ключевого кадра.
std::vector<uint8_t> ExtractParameterSets(const uint8_t* data, size_t size, Codec codec);
