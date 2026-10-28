#include "encode/video_encoder.h"
#include "common/log.h"
#include "encode/amf_encoder.h"
#include "encode/mf_encoder.h"
#include "encode/nvenc_encoder.h"

#include <stdexcept>

namespace {

enum class Backend { Nvenc, Amf, MfIntel, MfAny };

const char* BackendName(Backend b)
{
    switch (b) {
    case Backend::Nvenc: return "NVENC";
    case Backend::Amf: return "AMF";
    case Backend::MfIntel: return "Quick Sync";
    default: return "MF HW";
    }
}

std::vector<Backend> Order(GpuVendor vendor, EncoderPref pref)
{
    switch (pref) {
    case EncoderPref::Nvenc: return { Backend::Nvenc };
    case EncoderPref::Amf: return { Backend::Amf };
    case EncoderPref::Qsv: return { Backend::MfIntel };
    default: break;
    }
    switch (vendor) {
    case GpuVendor::Nvidia: return { Backend::Nvenc, Backend::MfAny };
    case GpuVendor::Amd: return { Backend::Amf, Backend::MfAny };
    case GpuVendor::Intel: return { Backend::MfIntel, Backend::MfAny };
    default: return { Backend::Nvenc, Backend::Amf, Backend::MfAny };
    }
}

} // namespace

std::unique_ptr<IVideoEncoder> CreateVideoEncoder(const D3DContext& d3d, const VideoStreamInfo& info, EncoderPref pref)
{
    std::string errors;
    for (const Backend b : Order(d3d.vendor, pref)) {
        try {
            switch (b) {
            case Backend::Nvenc: return std::make_unique<NvencEncoder>(d3d.device.get(), info);
            case Backend::Amf: return CreateAmfEncoder(d3d.device.get(), info);
            case Backend::MfIntel: return CreateMfEncoder(d3d, info, 0x8086);
            case Backend::MfAny: return CreateMfEncoder(d3d, info, 0);
            }
        } catch (const std::exception& e) {
            LogWarn("%s недоступен: %s", BackendName(b), e.what());
            errors += std::string(errors.empty() ? "" : "; ") + BackendName(b) + ": " + e.what();
        }
    }
    throw std::runtime_error("Нет аппаратного кодировщика (" + errors + ")");
}

std::vector<uint8_t> ExtractParameterSets(const uint8_t* data, size_t size, Codec codec)
{
    std::vector<uint8_t> out;
    size_t i = 0;
    auto findStart = [&](size_t from, size_t& codeLen) -> size_t {
        for (size_t p = from; p + 3 <= size; ++p) {
            if (data[p] == 0 && data[p + 1] == 0) {
                if (data[p + 2] == 1) {
                    codeLen = 3;
                    return p;
                }
                if (p + 4 <= size && data[p + 2] == 0 && data[p + 3] == 1) {
                    codeLen = 4;
                    return p;
                }
            }
        }
        codeLen = 0;
        return size;
    };

    size_t codeLen = 0;
    i = findStart(0, codeLen);
    while (i < size) {
        const size_t nalStart = i + codeLen;
        size_t nextLen = 0;
        const size_t next = findStart(nalStart, nextLen);
        if (nalStart < next) {
            const uint8_t h = data[nalStart];
            const int type = codec == Codec::H264 ? (h & 0x1F) : ((h >> 1) & 0x3F);
            const bool ps = codec == Codec::H264 ? (type == 7 || type == 8) : (type >= 32 && type <= 34);
            if (ps) {
                static constexpr uint8_t kStart[] = { 0, 0, 0, 1 };
                out.insert(out.end(), kStart, kStart + 4);
                out.insert(out.end(), data + nalStart, data + next);
            }
        }
        i = next;
        codeLen = nextLen;
    }
    return out;
}
