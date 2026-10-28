#include "encode/amf_encoder.h"
#include "common/hr.h"
#include "common/log.h"

#include <core/Factory.h>
#include <components/VideoEncoderHEVC.h>
#include <components/VideoEncoderVCE.h>

#include <algorithm>
#include <format>
#include <stdexcept>

namespace {

void Check(AMF_RESULT r, const char* what)
{
    if (r != AMF_OK)
        throw std::runtime_error(std::format("{}: AMF_RESULT {}", what, static_cast<int>(r)));
}

class AmfEncoder final : public IVideoEncoder {
public:
    AmfEncoder(ID3D11Device* device, const VideoStreamInfo& info)
        : info_(info), hevc_(info.codec == Codec::HEVC)
    {
        device->GetImmediateContext(context11_.put());

        lib_ = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib_)
            throw std::runtime_error("amfrt64.dll не найдена — нужен драйвер AMD с AMF");
        const auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(lib_, AMF_INIT_FUNCTION_NAME));
        if (!init)
            throw std::runtime_error("amfrt64.dll: нет AMFInit");
        Check(init(AMF_FULL_VERSION, &factory_), "AMFInit");

        Check(factory_->CreateContext(&context_), "AMFFactory::CreateContext");
        Check(context_->InitDX11(device, amf::AMF_DX11_1), "AMFContext::InitDX11");
        Check(factory_->CreateComponent(context_, hevc_ ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC, &encoder_),
              "AMFFactory::CreateComponent(encoder)");

        const amf_int64 bitrate = static_cast<amf_int64>(info_.bitrateKbps) * 1000;
        const amf_int64 gop = static_cast<amf_int64>(info_.fps) * std::max(info_.keyintSeconds, 1u);
        const AMFSize size = AMFConstructSize(static_cast<amf_int32>(info_.width), static_cast<amf_int32>(info_.height));
        const AMFRate rate = AMFConstructRate(info_.fps, 1);

        // Usage задаётся первым: он сбрасывает остальные параметры в профиль по умолчанию.
        if (hevc_) {
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, size);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, rate);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                                  info_.lowLatency ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED
                                                  : AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitrate);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bitrate);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, bitrate / info_.fps * 4);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, gop);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, static_cast<amf_int64>(1));
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
                                  AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED);
        } else {
            encoder_->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, size);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, rate);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_HIGH);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, info_.lowLatency ? AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED
                                                                                    : AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
            encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bitrate);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, bitrate / info_.fps * 4);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, gop);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, gop);
        }
        Check(encoder_->Init(amf::AMF_SURFACE_BGRA, static_cast<amf_int32>(info_.width),
                             static_cast<amf_int32>(info_.height)),
              "AMFComponent::Init");

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = info_.width;
        desc.Height = info_.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, input_.put()), "CreateTexture2D(amf input)");

        LogInfo("AMF: %s %ux%u@%u, %u кбит/с CBR, IDR каждые %lld кадров, low-latency", hevc_ ? "HEVC" : "H.264",
                info_.width, info_.height, info_.fps, info_.bitrateKbps, static_cast<long long>(gop));
    }

    ~AmfEncoder() override
    {
        if (encoder_) {
            encoder_->Terminate();
            encoder_->Release();
        }
        if (context_) {
            context_->Terminate();
            context_->Release();
        }
        if (lib_)
            FreeLibrary(lib_);
    }

    ID3D11Texture2D* InputTexture() const override { return input_.get(); }
    std::string Name() const override { return "AMD AMF"; }

    std::vector<uint8_t> SequenceHeader() override
    {
        amf::AMFVariant var;
        if (encoder_->GetProperty(hevc_ ? AMF_VIDEO_ENCODER_HEVC_EXTRADATA : AMF_VIDEO_ENCODER_EXTRADATA, &var) == AMF_OK &&
            var.type == amf::AMF_VARIANT_INTERFACE && var.pInterface) {
            amf::AMFBufferPtr buf(var.pInterface);
            if (buf && buf->GetSize() > 0) {
                const auto* p = static_cast<const uint8_t*>(buf->GetNative());
                return { p, p + buf->GetSize() };
            }
        }
        return {};
    }

    void Encode(int64_t pts, bool forceIdr, const PacketSink& sink) override
    {
        amf::AMFSurface* surface = nullptr;
        Check(context_->AllocSurface(amf::AMF_MEMORY_DX11, amf::AMF_SURFACE_BGRA, static_cast<amf_int32>(info_.width),
                                     static_cast<amf_int32>(info_.height), &surface),
              "AMFContext::AllocSurface");
        amf::AMFSurfacePtr owned(surface);
        surface->Release();  // AMFSurfacePtr уже держит ссылку

        // Своя копия кадра: кодер асинхронный, а InputTexture перезапишется следующим кадром.
        auto* native = static_cast<ID3D11Texture2D*>(owned->GetPlaneAt(0)->GetNative());
        context11_->CopyResource(native, input_.get());

        owned->SetPts(pts);
        if (forceIdr) {
            if (hevc_) {
                owned->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
                owned->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
            } else {
                owned->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
                owned->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
                owned->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
            }
        }

        AMF_RESULT r = encoder_->SubmitInput(owned);
        for (int i = 0; r == AMF_INPUT_FULL && i < 100; ++i) {
            Drain(sink, false);
            Sleep(1);
            r = encoder_->SubmitInput(owned);
        }
        if (r != AMF_OK && r != AMF_NEED_MORE_INPUT)
            Check(r, "AMFComponent::SubmitInput");
        ++pending_;

        // LOW_LATENCY отдаёт кадр почти сразу: ждём его (до ~50 мс), чтобы порядок пакетов
        // оставался «кадр на кадр», как у синхронного NVENC.
        for (int i = 0; i < 50 && pending_ > 0; ++i) {
            if (!Drain(sink, false))
                Sleep(1);
        }
    }

    void Flush() override
    {
        encoder_->Drain();
        PacketSink drop = [](const EncodedPacket&) {};
        for (int i = 0; i < 200; ++i) {
            const AMF_RESULT r = PullOne(drop);
            if (r == AMF_EOF)
                break;
            if (r != AMF_OK)
                Sleep(1);
        }
    }

private:
    // true — выдан хотя бы один пакет.
    bool Drain(const PacketSink& sink, bool)
    {
        bool any = false;
        while (PullOne(sink) == AMF_OK)
            any = true;
        return any;
    }

    AMF_RESULT PullOne(const PacketSink& sink)
    {
        amf::AMFData* data = nullptr;
        const AMF_RESULT r = encoder_->QueryOutput(&data);
        if (r != AMF_OK || !data)
            return r == AMF_OK ? AMF_REPEAT : r;
        amf::AMFDataPtr owned(data);
        data->Release();

        amf::AMFBufferPtr buf(owned);
        if (!buf)
            return AMF_OK;

        amf_int64 type = -1;
        owned->GetProperty(hevc_ ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE : AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);

        EncodedPacket pkt;
        pkt.data = { static_cast<const uint8_t*>(buf->GetNative()), buf->GetSize() };
        pkt.keyframe = hevc_ ? type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR
                             : type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
        pkt.pts = owned->GetPts();
        if (pending_ > 0)
            --pending_;
        sink(pkt);
        return AMF_OK;
    }

    VideoStreamInfo info_;
    bool hevc_;
    HMODULE lib_ = nullptr;
    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContext* context_ = nullptr;
    amf::AMFComponent* encoder_ = nullptr;
    winrt::com_ptr<ID3D11DeviceContext> context11_;
    winrt::com_ptr<ID3D11Texture2D> input_;
    int pending_ = 0;
};

} // namespace

std::unique_ptr<IVideoEncoder> CreateAmfEncoder(ID3D11Device* device, const VideoStreamInfo& info)
{
    return std::make_unique<AmfEncoder>(device, info);
}
