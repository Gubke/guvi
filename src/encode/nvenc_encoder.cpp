#include "encode/nvenc_encoder.h"
#include "common/hr.h"
#include "common/log.h"

#include <algorithm>
#include <cstdio>

namespace {

using PFN_CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using PFN_GetMaxSupportedVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);

void Check(NVENCSTATUS s, const char* what)
{
    if (s != NV_ENC_SUCCESS)
        throw std::runtime_error(std::format("{}: NVENCSTATUS {}", what, static_cast<int>(s)));
}

template <class F>
struct ScopeExit {
    F f;
    ~ScopeExit() { f(); }
};
template <class F>
ScopeExit(F) -> ScopeExit<F>;

} // namespace

NvencEncoder::NvencEncoder(ID3D11Device* device, const VideoStreamInfo& info)
    : info_(info)
{
    try {
        Init(device);
    } catch (...) {
        Release();
        throw;
    }
}

NvencEncoder::~NvencEncoder()
{
    Release();
}

void NvencEncoder::Init(ID3D11Device* device)
{
    lib_ = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib_)
        throw std::runtime_error("nvEncodeAPI64.dll не найдена — нужен драйвер NVIDIA с поддержкой NVENC");

    const auto getMaxVersion = reinterpret_cast<PFN_GetMaxSupportedVersion>(
        GetProcAddress(lib_, "NvEncodeAPIGetMaxSupportedVersion"));
    const auto createInstance = reinterpret_cast<PFN_CreateInstance>(
        GetProcAddress(lib_, "NvEncodeAPICreateInstance"));
    if (!getMaxVersion || !createInstance)
        throw std::runtime_error("nvEncodeAPI64.dll: нет нужных экспортов");

    uint32_t driverVersion = 0;
    Check(getMaxVersion(&driverVersion), "NvEncodeAPIGetMaxSupportedVersion");
    const uint32_t headerVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (headerVersion > driverVersion)
        throw std::runtime_error(std::format(
            "Драйвер поддерживает NVENC API {}.{}, а заголовок — {}.{}: обновите драйвер или возьмите старый nvEncodeAPI.h",
            driverVersion >> 4, driverVersion & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION));

    api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    Check(createInstance(&api_), "NvEncodeAPICreateInstance");

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device = device;
    open.apiVersion = NVENCAPI_VERSION;
    Check(api_.nvEncOpenEncodeSessionEx(&open, &encoder_), "nvEncOpenEncodeSessionEx");

    const bool h264 = info_.codec == Codec::H264;
    const GUID codecGuid = h264 ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;
    // Низкая задержка: P2 + ULTRA_LOW_LATENCY — меньше всего работы NVENC, когда игра грузит GPU.
    const GUID presetGuid = info_.lowLatency ? NV_ENC_PRESET_P2_GUID : NV_ENC_PRESET_P5_GUID;
    const NV_ENC_TUNING_INFO tuning =
        info_.lowLatency ? NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY : NV_ENC_TUNING_INFO_HIGH_QUALITY;

    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    Check(api_.nvEncGetEncodePresetConfigEx(encoder_, codecGuid, presetGuid, tuning, &preset),
          "nvEncGetEncodePresetConfigEx");

    NV_ENC_CONFIG cfg = preset.presetCfg;
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.profileGUID = h264 ? NV_ENC_H264_PROFILE_HIGH_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID;
    // Периодический IDR: replay-буфер и новые записи начинаются только с ключевого кадра.
    cfg.gopLength = info_.fps * std::max(info_.keyintSeconds, 1u);
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = info_.bitrateKbps * 1000;
    cfg.rcParams.maxBitRate = cfg.rcParams.averageBitRate;
    cfg.rcParams.vbvBufferSize = cfg.rcParams.averageBitRate / info_.fps * 4;
    cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;

    if (h264) {
        auto& c = cfg.encodeCodecConfig.h264Config;
        c.idrPeriod = cfg.gopLength;
        c.repeatSPSPPS = 1;
    } else {
        auto& c = cfg.encodeCodecConfig.hevcConfig;
        c.idrPeriod = cfg.gopLength;
        c.repeatSPSPPS = 1;
        c.disableSPSPPS = 0;
        c.outputAUD = 0;          // AUD в MP4 нельзя, из-за него первый кадр отвергается
        c.chromaFormatIDC = 1;    // 4:2:0, иначе пресет может оставить 0 и кадр не кодируется
    }

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = codecGuid;
    init.presetGUID = presetGuid;
    init.tuningInfo = tuning;
    init.encodeWidth = info_.width;
    init.encodeHeight = info_.height;
    init.darWidth = info_.width;
    init.darHeight = info_.height;
    init.maxEncodeWidth = info_.width;
    init.maxEncodeHeight = info_.height;
    init.frameRateNum = info_.fps;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.enableEncodeAsync = 0;
    init.encodeConfig = &cfg;
    Check(api_.nvEncInitializeEncoder(encoder_, &init), "nvEncInitializeEncoder");

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = info_.width;
    desc.Height = info_.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, input_.put()), "CreateTexture2D(nvenc input)");

    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = info_.width;
    reg.height = info_.height;
    reg.pitch = 0;
    reg.resourceToRegister = input_.get();
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;  // = DXGI_FORMAT_B8G8R8A8_UNORM
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    Check(api_.nvEncRegisterResource(encoder_, &reg), "nvEncRegisterResource");
    registered_ = reg.registeredResource;

    NV_ENC_CREATE_BITSTREAM_BUFFER bs{};
    bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    Check(api_.nvEncCreateBitstreamBuffer(encoder_, &bs), "nvEncCreateBitstreamBuffer");
    bitstream_ = bs.bitstreamBuffer;

    LogInfo("NVENC: %s %ux%u@%u, %u кбит/с CBR, IDR каждые %u с, P4/low-latency\n",
                h264 ? "H.264" : "HEVC", info_.width, info_.height, info_.fps, info_.bitrateKbps,
                cfg.gopLength / info_.fps);
}

void NvencEncoder::Release() noexcept
{
    if (encoder_) {
        if (bitstream_)
            api_.nvEncDestroyBitstreamBuffer(encoder_, bitstream_);
        if (registered_)
            api_.nvEncUnregisterResource(encoder_, registered_);
        api_.nvEncDestroyEncoder(encoder_);
    }
    bitstream_ = nullptr;
    registered_ = nullptr;
    encoder_ = nullptr;
    input_ = nullptr;
    if (lib_) {
        FreeLibrary(lib_);
        lib_ = nullptr;
    }
}

std::vector<uint8_t> NvencEncoder::SequenceHeader()
{
    std::vector<uint8_t> buf(1024);
    uint32_t size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD sp{};
    sp.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    sp.inBufferSize = static_cast<uint32_t>(buf.size());
    sp.spsppsBuffer = buf.data();
    sp.outSPSPPSPayloadSize = &size;
    Check(api_.nvEncGetSequenceParams(encoder_, &sp), "nvEncGetSequenceParams");
    buf.resize(size);
    return buf;
}

void NvencEncoder::Encode(int64_t pts, bool forceIdr, const PacketSink& sink)
{
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registered_;
    Check(api_.nvEncMapInputResource(encoder_, &map), "nvEncMapInputResource");
    ScopeExit unmap{ [&] { api_.nvEncUnmapInputResource(encoder_, map.mappedResource); } };

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = info_.width;
    pic.inputHeight = info_.height;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.outputBitstream = bitstream_;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = static_cast<uint64_t>(pts);
    if (forceIdr)
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    const NVENCSTATUS st = api_.nvEncEncodePicture(encoder_, &pic);
    if (st == NV_ENC_ERR_NEED_MORE_INPUT)
        return;
    Check(st, "nvEncEncodePicture");

    // Синхронный режим: Lock ждёт окончания кодирования, вход должен оставаться замапленным до этого.
    DrainBitstream(sink);
}

void NvencEncoder::DrainBitstream(const PacketSink& sink)
{
    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstream_;
    Check(api_.nvEncLockBitstream(encoder_, &lock), "nvEncLockBitstream");
    ScopeExit unlock{ [&] { api_.nvEncUnlockBitstream(encoder_, bitstream_); } };

    EncodedPacket pkt;
    pkt.data = { static_cast<const uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes };
    pkt.keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
    pkt.pts = static_cast<int64_t>(lock.outputTimeStamp);
    sink(pkt);
}

void NvencEncoder::Flush()
{
    // Без B-кадров и в синхронном режиме очередь пуста; EOS нужен для корректного закрытия сессии.
    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    Check(api_.nvEncEncodePicture(encoder_, &pic), "nvEncEncodePicture(EOS)");
}
