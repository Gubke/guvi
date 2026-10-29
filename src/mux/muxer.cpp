#include "mux/muxer.h"
#include "common/hr.h"
#include "common/log.h"
#include "common/mf_scope.h"

#include <unknwn.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

winrt::com_ptr<IMFMediaType> MakeVideoType(const VideoStreamInfo& v, const std::vector<uint8_t>& seq)
{
    winrt::com_ptr<IMFMediaType> type;
    ThrowIfFailed(MFCreateMediaType(type.put()), "MFCreateMediaType");
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, v.codec == Codec::H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC);
    type->SetUINT32(MF_MT_AVG_BITRATE, v.bitrateKbps * 1000);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, v.width, v.height);
    MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, v.fps, 1);
    MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // Annex B, как отдаёт NVENC. HVCC и длины NAL дают E_UNEXPECTED на WriteSample/Finalize.
    if (!seq.empty())
        type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seq.data(), static_cast<UINT32>(seq.size()));
    return type;
}

winrt::com_ptr<IMFMediaType> MakeAacType(const AudioStreamInfo& a)
{
    winrt::com_ptr<IMFMediaType> type;
    ThrowIfFailed(MFCreateMediaType(type.put()), "MFCreateMediaType");
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, a.sampleRate);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, a.channels);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, a.bitrateKbps * 1000 / 8);
    type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
    type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    if (!a.aacUserData.empty())
        type->SetBlob(MF_MT_USER_DATA, a.aacUserData.data(), static_cast<UINT32>(a.aacUserData.size()));
    return type;
}

class MfMp4Muxer final : public IMuxer {
public:
    MfMp4Muxer(const std::wstring& path, const StreamSetup& setup, int64_t timeOffset, bool crashSafe)
        : offset_(timeOffset), crashSafe_(crashSafe && setup.video.codec != Codec::HEVC)
    {
        const auto vt = MakeVideoType(setup.video, setup.videoSequenceHeader);
        winrt::com_ptr<IMFMediaType> at;
        if (setup.audio)
            at = MakeAacType(*setup.audio);

        // FMPEG4 отвечает MF_E_INVALIDMEDIATYPE на HEVC. Обычный приёмник принимает Annex B.
        if (setup.video.codec == Codec::HEVC)
            LogInfo("HEVC: обычный MP4, фрагментированный контейнер этот кодек не принимает");

        if (crashSafe_) {
            bool opened = false;
            try {
                ThrowIfFailed(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE,
                                           path.c_str(), stream_.put()),
                              "MFCreateFile");
                if (!OpenFragmented(vt.get(), at.get())) {
                    writer_ = nullptr;
                    stream_ = nullptr;
                    hasAudio_ = false;
                    ThrowIfFailed(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE,
                                               path.c_str(), stream_.put()),
                                  "MFCreateFile");
                    OpenFragmentedFallback(vt.get(), at.get());
                }
                if (stream_)
                    stream_->Flush();
                opened = true;
            } catch (const std::exception& e) {
                writer_ = nullptr;
                stream_ = nullptr;
                hasAudio_ = false;
                LogWarn("Фрагментированный MP4 недоступен (%s), пишу обычный MP4", e.what());
            }
            if (!opened)
                OpenClassic(path, vt.get(), at.get());
        } else {
            OpenClassic(path, vt.get(), at.get());
        }
    }

    ~MfMp4Muxer() override
    {
        if (writer_)
            writer_->Finalize();
        if (stream_)
            stream_->Flush();
    }

    bool SupportsAudio() const override { return hasAudio_; }

    void Write(const Packet& pkt) override
    {
        const bool audio = pkt.stream == StreamKind::Audio;
        if (audio && !hasAudio_)
            return;
        const int64_t pts = pkt.pts - offset_;
        if (pts < 0)
            return;

        SubmitSample(audio, pts, pkt.duration, pkt.keyframe, pkt.data.data(), static_cast<DWORD>(pkt.data.size()));
    }

    void Finish() override
    {
        if (!writer_)
            return;
        const HRESULT hr = writer_->Finalize();
        writer_ = nullptr;
        if (stream_) {
            stream_->Flush();
            stream_ = nullptr;
        }
        ThrowIfFailed(hr, "IMFSinkWriter::Finalize");
    }

private:
    void SubmitSample(bool audio, int64_t pts, int64_t duration, bool keyframe, const uint8_t* bytes, DWORD size)
    {
        winrt::com_ptr<IMFMediaBuffer> buffer;
        ThrowIfFailed(MFCreateMemoryBuffer(size, buffer.put()), "MFCreateMemoryBuffer");
        BYTE* dst = nullptr;
        ThrowIfFailed(buffer->Lock(&dst, nullptr, nullptr), "IMFMediaBuffer::Lock");
        std::memcpy(dst, bytes, size);
        buffer->Unlock();
        buffer->SetCurrentLength(size);

        winrt::com_ptr<IMFSample> sample;
        ThrowIfFailed(MFCreateSample(sample.put()), "MFCreateSample");
        sample->AddBuffer(buffer.get());
        sample->SetSampleTime(pts);
        sample->SetSampleDuration(duration);
        if (keyframe)
            sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        ThrowIfFailed(writer_->WriteSample(audio ? audioStream_ : videoStream_, sample.get()),
                      "IMFSinkWriter::WriteSample");
        if (crashSafe_ && !audio && stream_ && pts - lastFlushPts_ >= kTimescale) {
            stream_->Flush();
            lastFlushPts_ = pts;
        }
    }

    void OpenClassic(const std::wstring& path, IMFMediaType* video, IMFMediaType* audio)
    {
        writer_ = nullptr;
        stream_ = nullptr;
        hasAudio_ = false;
        winrt::com_ptr<IMFAttributes> attrs;
        ThrowIfFailed(MFCreateAttributes(attrs.put(), 2), "MFCreateAttributes");
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        ThrowIfFailed(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.get(), writer_.put()),
                      "MFCreateSinkWriterFromURL");
        AddStreams(video, audio);
        ThrowIfFailed(writer_->BeginWriting(), "IMFSinkWriter::BeginWriting");
    }

    void AddStreams(IMFMediaType* video, IMFMediaType* audio)
    {
        // Одинаковые входной и выходной типы → Sink Writer не вставляет кодеры, пишет сэмплы как есть.
        ThrowIfFailed(writer_->AddStream(video, &videoStream_), "AddStream(video)");
        ThrowIfFailed(writer_->SetInputMediaType(videoStream_, video, nullptr), "SetInputMediaType(video)");
        if (audio) {
            ThrowIfFailed(writer_->AddStream(audio, &audioStream_), "AddStream(audio)");
            ThrowIfFailed(writer_->SetInputMediaType(audioStream_, audio, nullptr), "SetInputMediaType(audio)");
            hasAudio_ = true;
        }
    }

    bool OpenFragmented(IMFMediaType* video, IMFMediaType* audio)
    {
        winrt::com_ptr<IMFAttributes> sinkAttr;
        if (FAILED(MFCreateAttributes(sinkAttr.put(), 2)))
            return false;
        // Фрагмент раз в секунду: при обрыве питания пропадает только хвост, не весь файл.
        sinkAttr->SetUINT64(MF_MPEG4SINK_MIN_FRAGMENT_DURATION, static_cast<UINT64>(kTimescale));

        winrt::com_ptr<IMFMediaSink> sink;
        if (FAILED(MFCreateMuxSink(MFTranscodeContainerType_FMPEG4, sinkAttr.get(), stream_.get(), sink.put())))
            return false;

        winrt::com_ptr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(attrs.put(), 1)))
            return false;
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        if (FAILED(MFCreateSinkWriterFromMediaSink(sink.get(), attrs.get(), writer_.put())))
            return false;
        if (FAILED(writer_->AddStream(video, &videoStream_)) ||
            FAILED(writer_->SetInputMediaType(videoStream_, video, nullptr)))
            return false;
        if (audio) {
            if (FAILED(writer_->AddStream(audio, &audioStream_)) ||
                FAILED(writer_->SetInputMediaType(audioStream_, audio, nullptr)))
                return false;
            hasAudio_ = true;
        }
        return SUCCEEDED(writer_->BeginWriting());
    }

    void OpenFragmentedFallback(IMFMediaType* video, IMFMediaType* audio)
    {
        winrt::com_ptr<IMFMediaSink> sink;
        ThrowIfFailed(MFCreateFMPEG4MediaSink(stream_.get(), video, audio, sink.put()), "MFCreateFMPEG4MediaSink");
        if (auto attr = sink.try_as<IMFAttributes>())
            attr->SetUINT64(MF_MPEG4SINK_MIN_FRAGMENT_DURATION, static_cast<UINT64>(kTimescale));

        winrt::com_ptr<IMFAttributes> attrs;
        ThrowIfFailed(MFCreateAttributes(attrs.put(), 1), "MFCreateAttributes");
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        ThrowIfFailed(MFCreateSinkWriterFromMediaSink(sink.get(), attrs.get(), writer_.put()),
                      "MFCreateSinkWriterFromMediaSink");
        videoStream_ = 0;
        ThrowIfFailed(writer_->SetInputMediaType(videoStream_, video, nullptr), "SetInputMediaType(video)");
        if (audio) {
            audioStream_ = 1;
            ThrowIfFailed(writer_->SetInputMediaType(audioStream_, audio, nullptr), "SetInputMediaType(audio)");
            hasAudio_ = true;
        }
        ThrowIfFailed(writer_->BeginWriting(), "IMFSinkWriter::BeginWriting");
    }

    MfScope mf_;
    winrt::com_ptr<IMFSinkWriter> writer_;
    winrt::com_ptr<IMFByteStream> stream_;
    DWORD videoStream_ = 0;
    DWORD audioStream_ = 0;
    bool hasAudio_ = false;
    bool crashSafe_ = false;
    int64_t offset_ = 0;
    int64_t lastFlushPts_ = 0;
};

class RawMuxer final : public IMuxer {
public:
    explicit RawMuxer(const std::wstring& path)
        : out_(std::filesystem::path(path), std::ios::binary | std::ios::trunc)
    {
        if (!out_)
            throw std::runtime_error("Не удалось открыть выходной файл");
    }

    bool SupportsAudio() const override { return false; }

    void Write(const Packet& pkt) override
    {
        if (pkt.stream != StreamKind::Video)
            return;
        out_.write(reinterpret_cast<const char*>(pkt.data.data()), static_cast<std::streamsize>(pkt.data.size()));
        if (!out_)
            throw std::runtime_error("Ошибка записи в файл");
    }

    void Finish() override { out_.close(); }

private:
    std::ofstream out_;
};

} // namespace

std::unique_ptr<IMuxer> CreateMuxer(const std::wstring& path, const StreamSetup& setup, int64_t timeOffset, bool crashSafe)
{
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });

    if (ext == L".mp4")
        return std::make_unique<MfMp4Muxer>(path, setup, timeOffset, crashSafe);

    const bool rawH264 = ext == L".h264" || ext == L".264";
    const bool rawHevc = ext == L".hevc" || ext == L".h265" || ext == L".265";
    if (rawH264 || rawHevc) {
        if (rawH264 != (setup.video.codec == Codec::H264))
            throw std::runtime_error("Расширение файла не совпадает с --codec");
        return std::make_unique<RawMuxer>(path);
    }
    throw std::runtime_error("Поддерживаются .mp4, .h264/.264, .hevc/.h265/.265");
}
