#include "audio/aac_encoder.h"
#include "common/hr.h"

#include <mfapi.h>
#include <mferror.h>

#include <cstring>
#include <iterator>

namespace {

constexpr UINT32 kAacLcProfileLevel = 0x29;

winrt::com_ptr<IMFTransform> CreateAacMft()
{
    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Audio, MFAudioFormat_PCM };
    MFT_REGISTER_TYPE_INFO out{ MFMediaType_Audio, MFAudioFormat_AAC };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    ThrowIfFailed(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                            &in, &out, &activates, &count),
                  "MFTEnumEx(AAC)");

    winrt::com_ptr<IMFTransform> mft;
    HRESULT hr = count ? activates[0]->ActivateObject(IID_PPV_ARGS(mft.put())) : MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; ++i)
        activates[i]->Release();
    CoTaskMemFree(activates);
    ThrowIfFailed(hr, "AAC encoder MFT");
    return mft;
}

// Если MFT не отдал MF_MT_USER_DATA: 12 байт HEAACWAVEINFO (payload=0, profile=0x29) + ASC AAC-LC.
std::vector<uint8_t> DefaultUserData(uint32_t sampleRate, uint32_t channels)
{
    static constexpr uint32_t kRates[] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                           24000, 22050, 16000, 12000, 11025, 8000 };
    uint32_t freqIndex = 3;
    for (uint32_t i = 0; i < std::size(kRates); ++i)
        if (kRates[i] == sampleRate)
            freqIndex = i;
    const uint16_t asc = static_cast<uint16_t>((2u << 11) | (freqIndex << 7) | (channels << 3));
    return { 0, 0, kAacLcProfileLevel, 0, 0, 0, 0, 0, 0, 0, 0, 0,
             static_cast<uint8_t>(asc >> 8), static_cast<uint8_t>(asc & 0xFF) };
}

} // namespace

AacEncoder::AacEncoder(uint32_t sampleRate, uint32_t channels, uint32_t bitrateKbps)
{
    info_.sampleRate = sampleRate;
    info_.channels = channels;
    info_.bitrateKbps = bitrateKbps;
    frameDuration_ = static_cast<int64_t>(kFrameSamples) * kTimescale / sampleRate;

    mft_ = CreateAacMft();

    // У MS AAC Encoder выходной тип задаётся раньше входного.
    winrt::com_ptr<IMFMediaType> outType;
    ThrowIfFailed(MFCreateMediaType(outType.put()), "MFCreateMediaType");
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrateKbps * 1000 / 8);
    outType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    outType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, kAacLcProfileLevel);
    ThrowIfFailed(mft_->SetOutputType(0, outType.get(), 0), "AAC SetOutputType");

    winrt::com_ptr<IMFMediaType> inType;
    ThrowIfFailed(MFCreateMediaType(inType.put()), "MFCreateMediaType");
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * channels * 2);
    ThrowIfFailed(mft_->SetInputType(0, inType.get(), 0), "AAC SetInputType");

    winrt::com_ptr<IMFMediaType> current;
    if (SUCCEEDED(mft_->GetOutputCurrentType(0, current.put()))) {
        UINT32 size = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_USER_DATA, &size)) && size > 0) {
            info_.aacUserData.resize(size);
            current->GetBlob(MF_MT_USER_DATA, info_.aacUserData.data(), size, nullptr);
        }
    }
    if (info_.aacUserData.empty())
        info_.aacUserData = DefaultUserData(sampleRate, channels);

    MFT_OUTPUT_STREAM_INFO si{};
    ThrowIfFailed(mft_->GetOutputStreamInfo(0, &si), "AAC GetOutputStreamInfo");
    providesSamples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outputSize_ = si.cbSize ? si.cbSize : 8192;
    if (!providesSamples_) {
        ThrowIfFailed(MFCreateSample(outSample_.put()), "MFCreateSample");
        ThrowIfFailed(MFCreateMemoryBuffer(outputSize_, outBuffer_.put()), "MFCreateMemoryBuffer");
        outSample_->AddBuffer(outBuffer_.get());
    }

    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

void AacEncoder::Encode(const int16_t* pcm, uint32_t frames, int64_t pts, const PacketFn& out)
{
    const DWORD bytes = frames * info_.channels * sizeof(int16_t);
    winrt::com_ptr<IMFMediaBuffer> buffer;
    ThrowIfFailed(MFCreateMemoryBuffer(bytes, buffer.put()), "MFCreateMemoryBuffer");
    BYTE* dst = nullptr;
    ThrowIfFailed(buffer->Lock(&dst, nullptr, nullptr), "IMFMediaBuffer::Lock");
    std::memcpy(dst, pcm, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);

    winrt::com_ptr<IMFSample> sample;
    ThrowIfFailed(MFCreateSample(sample.put()), "MFCreateSample");
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(pts);
    sample->SetSampleDuration(static_cast<int64_t>(frames) * kTimescale / info_.sampleRate);

    if (nextPts_ < 0)
        nextPts_ = pts;

    HRESULT hr = mft_->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        PullOutput(out);
        hr = mft_->ProcessInput(0, sample.get(), 0);
    }
    ThrowIfFailed(hr, "AAC ProcessInput");
    PullOutput(out);
}

void AacEncoder::Drain(const PacketFn& out)
{
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    PullOutput(out);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
}

void AacEncoder::PullOutput(const PacketFn& out)
{
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = 0;
        if (!providesSamples_) {
            outBuffer_->SetCurrentLength(0);
            ob.pSample = outSample_.get();
        }
        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents)
            ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            return;
        ThrowIfFailed(hr, "AAC ProcessOutput");

        winrt::com_ptr<IMFSample> produced;
        if (providesSamples_)
            produced.attach(ob.pSample);
        else
            produced.copy_from(ob.pSample);
        if (!produced)
            continue;

        winrt::com_ptr<IMFMediaBuffer> contiguous;
        ThrowIfFailed(produced->ConvertToContiguousBuffer(contiguous.put()), "ConvertToContiguousBuffer");
        BYTE* src = nullptr;
        DWORD len = 0;
        ThrowIfFailed(contiguous->Lock(&src, nullptr, &len), "IMFMediaBuffer::Lock");

        auto pkt = std::make_shared<Packet>();
        pkt->stream = StreamKind::Audio;
        pkt->keyframe = true;
        pkt->duration = frameDuration_;
        pkt->data.assign(src, src + len);
        contiguous->Unlock();

        LONGLONG t = 0;
        pkt->pts = SUCCEEDED(produced->GetSampleTime(&t)) ? t : nextPts_;
        nextPts_ = pkt->pts + frameDuration_;

        if (!pkt->data.empty())
            out(std::move(pkt));
    }
}
