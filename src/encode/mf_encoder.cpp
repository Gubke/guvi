#include "encode/mf_encoder.h"
#include "common/clock.h"
#include "common/hr.h"
#include "common/log.h"
#include "common/mf_scope.h"

#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <format>
#include <stdexcept>

namespace {

constexpr int kPoolSize = 8;

std::wstring GetString(IMFAttributes* a, const GUID& key)
{
    UINT32 len = 0;
    if (FAILED(a->GetStringLength(key, &len)))
        return {};
    std::wstring s(len + 1, L'\0');
    if (FAILED(a->GetString(key, s.data(), len + 1, &len)))
        return {};
    s.resize(len);
    return s;
}

uint32_t VendorIdFromAttr(const std::wstring& v)
{
    // Формат "VEN_8086".
    const auto pos = v.find(L"VEN_");
    if (pos == std::wstring::npos)
        return 0;
    return static_cast<uint32_t>(std::wcstoul(v.c_str() + pos + 4, nullptr, 16));
}

std::string VendorLabel(uint32_t id)
{
    switch (id) {
    case 0x8086: return "Intel Quick Sync";
    case 0x1002: case 0x1022: return "AMD (Media Foundation)";
    case 0x10DE: return "NVIDIA (Media Foundation)";
    default: return "Media Foundation HW";
    }
}

void SetCodecValue(ICodecAPI* api, const GUID& key, UINT32 value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&key, &v);  // не все MFT поддерживают все параметры — не критично
}

void SetCodecBool(ICodecAPI* api, const GUID& key, bool value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&key, &v);
}

class MfEncoder final : public IVideoEncoder {
public:
    MfEncoder(const D3DContext& d3d, const VideoStreamInfo& info, uint32_t requireVendorId)
        : info_(info), device_(d3d.device), context_(d3d.context)
    {
        OpenMft(d3d, requireVendorId);
        ConfigureTypes();
        CreateConverter();

        ThrowIfFailed(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MFT BEGIN_STREAMING");
        ThrowIfFailed(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MFT START_OF_STREAM");

        LogInfo("%s: %s %ux%u@%u, %u кбит/с CBR, IDR каждые %u с", name_.c_str(),
                info_.codec == Codec::H264 ? "H.264" : "HEVC", info_.width, info_.height, info_.fps,
                info_.bitrateKbps, std::max(info_.keyintSeconds, 1u));
    }

    ~MfEncoder() override
    {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
            if (auto shutdown = mft_.try_as<IMFShutdown>())
                shutdown->Shutdown();
        }
    }

    ID3D11Texture2D* InputTexture() const override { return input_.get(); }
    std::string Name() const override { return name_; }

    std::vector<uint8_t> SequenceHeader() override
    {
        if (!sequenceHeader_.empty())
            return sequenceHeader_;

        // Некоторые MFT кладут SPS/PPS в выходной тип.
        winrt::com_ptr<IMFMediaType> current;
        if (SUCCEEDED(mft_->GetOutputCurrentType(outId_, current.put()))) {
            UINT32 size = 0;
            if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
                sequenceHeader_.resize(size);
                current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader_.data(), size, nullptr);
                return sequenceHeader_;
            }
        }

        // Иначе — пробный чёрный IDR-кадр: параметры берём из его NAL-ов, сам пакет выбрасываем.
        const float black[4] = { 0, 0, 0, 1 };
        context_->ClearRenderTargetView(inputRtv_.get(), black);
        for (int attempt = 0; attempt < 5 && sequenceHeader_.empty(); ++attempt) {
            Encode(primePts_, true, [&](const EncodedPacket& p) {
                if (sequenceHeader_.empty())
                    sequenceHeader_ = ExtractParameterSets(p.data.data(), p.data.size(), info_.codec);
            });
            primePts_ += kTimescale / info_.fps;
        }
        if (sequenceHeader_.empty())
            throw std::runtime_error(name_ + ": не удалось получить SPS/PPS");
        return sequenceHeader_;
    }

    void Encode(int64_t pts, bool forceIdr, const PacketSink& sink) override
    {
        // Пробные кадры имеют маленькие PTS; реальные должны идти строго после них.
        pts = std::max(pts, lastPts_ + 1);
        lastPts_ = pts;

        auto& slot = pool_[static_cast<size_t>(next_)];
        next_ = (next_ + 1) % kPoolSize;
        ThrowIfFailed(videoContext_->VideoProcessorBlt(processor_.get(), slot.view.get(), 0, 1, &stream_),
                      "VideoProcessorBlt");

        winrt::com_ptr<IMFMediaBuffer> buffer;
        ThrowIfFailed(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), slot.texture.get(), 0, FALSE, buffer.put()),
                      "MFCreateDXGISurfaceBuffer");
        if (auto b2 = buffer.try_as<IMF2DBuffer>()) {
            DWORD len = 0;
            if (SUCCEEDED(b2->GetContiguousLength(&len)))
                buffer->SetCurrentLength(len);
        }
        winrt::com_ptr<IMFSample> sample;
        ThrowIfFailed(MFCreateSample(sample.put()), "MFCreateSample");
        sample->AddBuffer(buffer.get());
        sample->SetSampleTime(pts);
        sample->SetSampleDuration(kTimescale / info_.fps);

        if (forceIdr && codecApi_)
            SetCodecValue(codecApi_.get(), CODECAPI_AVEncVideoForceKeyFrame, 1);

        // Асинхронный MFT: вход можно отдать только после METransformNeedInput.
        const int64_t freq = QpcFrequency();
        const int64_t inputDeadline = QpcNow() + freq / 5;
        while (needInput_ == 0) {
            if (!PumpEvents(sink) && QpcNow() > inputDeadline)
                throw std::runtime_error(name_ + ": кодировщик не принимает кадры");
            if (needInput_ == 0)
                Sleep(1);
        }
        --needInput_;
        ThrowIfFailed(mft_->ProcessInput(inId_, sample.get(), 0), "MFT ProcessInput");
        ++pending_;

        // Ждём выход этого кадра (до ~50 мс), чтобы пакеты шли «кадр на кадр».
        const int64_t outputDeadline = QpcNow() + freq / 20;
        while (pending_ > 0 && QpcNow() < outputDeadline) {
            if (!PumpEvents(sink))
                Sleep(1);
        }
    }

    void Flush() override
    {
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        const PacketSink drop = [](const EncodedPacket&) {};
        const int64_t deadline = QpcNow() + QpcFrequency() / 2;
        while (!drained_ && QpcNow() < deadline) {
            if (!PumpEvents(drop))
                Sleep(1);
        }
    }

private:
    struct Slot {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11VideoProcessorOutputView> view;
    };

    void OpenMft(const D3DContext& d3d, uint32_t requireVendorId)
    {
        MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_NV12 };
        MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, info_.codec == Codec::H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        ThrowIfFailed(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in,
                                &out, &activates, &count),
                      "MFTEnumEx(video encoder)");

        const UINT64 luid = (static_cast<UINT64>(static_cast<uint32_t>(d3d.adapterLuid.HighPart)) << 32) |
                            d3d.adapterLuid.LowPart;
        HRESULT hr = MF_E_TOPO_CODEC_NOT_FOUND;
        for (UINT32 i = 0; i < count && !mft_; ++i) {
            const uint32_t vendor = VendorIdFromAttr(GetString(activates[i], MFT_ENUM_HARDWARE_VENDOR_ID_Attribute));
            if (requireVendorId && vendor != requireVendorId)
                continue;
            UINT64 mftLuid = 0;
            if (luid && SUCCEEDED(activates[i]->GetUINT64(MFT_ENUM_ADAPTER_LUID, &mftLuid)) && mftLuid != luid)
                continue;  // MFT другой видеокарты — общие текстуры туда не попадут
            hr = activates[i]->ActivateObject(IID_PPV_ARGS(mft_.put()));
            if (SUCCEEDED(hr)) {
                name_ = VendorLabel(vendor);
                const auto friendly = GetString(activates[i], MFT_FRIENDLY_NAME_Attribute);
                if (!friendly.empty())
                    LogInfo("MFT: %s", WideToUtf8(friendly).c_str());
            }
        }
        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        CoTaskMemFree(activates);
        ThrowIfFailed(mft_ ? S_OK : hr, "аппаратный MFT-кодировщик не найден");

        winrt::com_ptr<IMFAttributes> attrs;
        ThrowIfFailed(mft_->GetAttributes(attrs.put()), "MFT GetAttributes");
        UINT32 async = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
            ThrowIfFailed(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "MF_TRANSFORM_ASYNC_UNLOCK");
        } else {
            throw std::runtime_error("ожидался асинхронный аппаратный MFT");
        }
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        events_ = mft_.as<IMFMediaEventGenerator>();

        UINT token = 0;
        ThrowIfFailed(MFCreateDXGIDeviceManager(&token, manager_.put()), "MFCreateDXGIDeviceManager");
        ThrowIfFailed(manager_->ResetDevice(d3d.device.get(), token), "IMFDXGIDeviceManager::ResetDevice");
        ThrowIfFailed(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager_.get())),
                      "MFT SET_D3D_MANAGER");

        DWORD inId = 0, outId = 0;
        if (SUCCEEDED(mft_->GetStreamIDs(1, &inId, 1, &outId))) {
            inId_ = inId;
            outId_ = outId;
        }
        codecApi_ = mft_.try_as<ICodecAPI>();
    }

    void ConfigureTypes()
    {
        const UINT32 gop = info_.fps * std::max(info_.keyintSeconds, 1u);
        if (codecApi_) {
            SetCodecValue(codecApi_.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            SetCodecValue(codecApi_.get(), CODECAPI_AVEncCommonMeanBitRate, info_.bitrateKbps * 1000);
            SetCodecValue(codecApi_.get(), CODECAPI_AVEncMPVGOPSize, gop);
            SetCodecValue(codecApi_.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            SetCodecBool(codecApi_.get(), CODECAPI_AVLowLatencyMode, info_.lowLatency);
            SetCodecValue(codecApi_.get(), CODECAPI_AVEncCommonQualityVsSpeed, info_.lowLatency ? 20u : 80u);
        }

        const bool h264 = info_.codec == Codec::H264;
        winrt::com_ptr<IMFMediaType> outType;
        ThrowIfFailed(MFCreateMediaType(outType.put()), "MFCreateMediaType");
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, h264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC);
        outType->SetUINT32(MF_MT_AVG_BITRATE, info_.bitrateKbps * 1000);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, h264 ? eAVEncH264VProfile_High : eAVEncH265VProfile_Main_420_8);
        MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, info_.width, info_.height);
        MFSetAttributeRatio(outType.get(), MF_MT_FRAME_RATE, info_.fps, 1);
        MFSetAttributeRatio(outType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        ThrowIfFailed(mft_->SetOutputType(outId_, outType.get(), 0), "MFT SetOutputType");

        winrt::com_ptr<IMFMediaType> inType;
        ThrowIfFailed(MFCreateMediaType(inType.put()), "MFCreateMediaType");
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inType.get(), MF_MT_FRAME_SIZE, info_.width, info_.height);
        MFSetAttributeRatio(inType.get(), MF_MT_FRAME_RATE, info_.fps, 1);
        MFSetAttributeRatio(inType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        ThrowIfFailed(mft_->SetInputType(inId_, inType.get(), 0), "MFT SetInputType");
    }

    void CreateConverter()
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = info_.width;
        desc.Height = info_.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(device_->CreateTexture2D(&desc, nullptr, input_.put()), "CreateTexture2D(mf input)");
        ThrowIfFailed(device_->CreateRenderTargetView(input_.get(), nullptr, inputRtv_.put()), "CreateRenderTargetView");

        videoDevice_ = device_.as<ID3D11VideoDevice>();
        videoContext_ = context_.as<ID3D11VideoContext>();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = cd.OutputWidth = info_.width;
        cd.InputHeight = cd.OutputHeight = info_.height;
        cd.InputFrameRate = cd.OutputFrameRate = { info_.fps, 1 };
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        ThrowIfFailed(videoDevice_->CreateVideoProcessorEnumerator(&cd, enumerator_.put()),
                      "CreateVideoProcessorEnumerator");
        ThrowIfFailed(videoDevice_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()), "CreateVideoProcessor");

        // RGB full range → YCbCr BT.709 limited range.
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
        inCs.RGB_Range = 0;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
        outCs.YCbCr_Matrix = 1;
        outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &inCs);
        videoContext_->VideoProcessorSetOutputColorSpace(processor_.get(), &outCs);
        videoContext_->VideoProcessorSetStreamFrameFormat(processor_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ThrowIfFailed(videoDevice_->CreateVideoProcessorInputView(input_.get(), enumerator_.get(), &ivd, inputView_.put()),
                      "CreateVideoProcessorInputView");

        D3D11_TEXTURE2D_DESC nv12 = desc;
        nv12.Format = DXGI_FORMAT_NV12;
        nv12.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        for (auto& s : pool_) {
            ThrowIfFailed(device_->CreateTexture2D(&nv12, nullptr, s.texture.put()), "CreateTexture2D(NV12)");
            ThrowIfFailed(videoDevice_->CreateVideoProcessorOutputView(s.texture.get(), enumerator_.get(), &ovd, s.view.put()),
                          "CreateVideoProcessorOutputView");
        }

        stream_.Enable = TRUE;
        stream_.pInputSurface = inputView_.get();
    }

    // false — событий не было.
    bool PumpEvents(const PacketSink& sink)
    {
        bool any = false;
        for (;;) {
            winrt::com_ptr<IMFMediaEvent> ev;
            const HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, ev.put());
            if (hr == MF_E_NO_EVENTS_AVAILABLE)
                return any;
            ThrowIfFailed(hr, "MFT GetEvent");
            any = true;
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            if (type == METransformNeedInput)
                ++needInput_;
            else if (type == METransformHaveOutput)
                PullOutput(sink);
            else if (type == METransformDrainComplete)
                drained_ = true;
        }
    }

    void PullOutput(const PacketSink& sink)
    {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = outId_;
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents)
            ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Кодер уточнил выходной тип (например, добавил SPS/PPS) — принять первый доступный.
            winrt::com_ptr<IMFMediaType> t;
            if (SUCCEEDED(mft_->GetOutputAvailableType(outId_, 0, t.put())))
                mft_->SetOutputType(outId_, t.get(), 0);
            if (ob.pSample)
                ob.pSample->Release();
            return;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            return;
        ThrowIfFailed(hr, "MFT ProcessOutput");

        winrt::com_ptr<IMFSample> produced;
        produced.attach(ob.pSample);
        if (!produced)
            return;
        winrt::com_ptr<IMFMediaBuffer> contiguous;
        ThrowIfFailed(produced->ConvertToContiguousBuffer(contiguous.put()), "ConvertToContiguousBuffer");
        BYTE* data = nullptr;
        DWORD len = 0;
        ThrowIfFailed(contiguous->Lock(&data, nullptr, &len), "IMFMediaBuffer::Lock");

        EncodedPacket pkt;
        pkt.data = { data, len };
        LONGLONG t = 0;
        pkt.pts = SUCCEEDED(produced->GetSampleTime(&t)) ? t : lastPts_;
        UINT32 clean = 0;
        pkt.keyframe = SUCCEEDED(produced->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean;
        if (pending_ > 0)
            --pending_;
        try {
            sink(pkt);
        } catch (...) {
            contiguous->Unlock();
            throw;
        }
        contiguous->Unlock();
    }

    VideoStreamInfo info_;
    MfScope mf_;
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    std::string name_ = "Media Foundation HW";

    winrt::com_ptr<IMFTransform> mft_;
    winrt::com_ptr<IMFMediaEventGenerator> events_;
    winrt::com_ptr<ICodecAPI> codecApi_;
    winrt::com_ptr<IMFDXGIDeviceManager> manager_;
    DWORD inId_ = 0;
    DWORD outId_ = 0;

    winrt::com_ptr<ID3D11Texture2D> input_;
    winrt::com_ptr<ID3D11RenderTargetView> inputRtv_;
    winrt::com_ptr<ID3D11VideoDevice> videoDevice_;
    winrt::com_ptr<ID3D11VideoContext> videoContext_;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
    winrt::com_ptr<ID3D11VideoProcessor> processor_;
    winrt::com_ptr<ID3D11VideoProcessorInputView> inputView_;
    D3D11_VIDEO_PROCESSOR_STREAM stream_{};
    std::array<Slot, kPoolSize> pool_;
    int next_ = 0;

    int needInput_ = 0;
    int pending_ = 0;
    bool drained_ = false;
    int64_t lastPts_ = -1;
    int64_t primePts_ = 0;
    std::vector<uint8_t> sequenceHeader_;
};

} // namespace

std::unique_ptr<IVideoEncoder> CreateMfEncoder(const D3DContext& d3d, const VideoStreamInfo& info,
                                               uint32_t requireVendorId)
{
    return std::make_unique<MfEncoder>(d3d, info, requireVendorId);
}
