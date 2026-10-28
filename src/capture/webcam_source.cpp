#include "capture/webcam_source.h"
#include "common/hr.h"
#include "common/log.h"

#include <unknwn.h>
#include <dshow.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstring>
#include <functional>

// Sample Grabber из qedit.dll: заголовок qedit.h убран из новых SDK, а сам компонент в Windows остался.
// Объявления — по публичной документации DirectShow.
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer, long bufferLen) = 0;
};

MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* bufferSize, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* callback, long whichMethod) = 0;
};

namespace {

constexpr CLSID kClsidSampleGrabber = { 0xC1F400A0, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };
constexpr CLSID kClsidNullRenderer = { 0xC1F400A4, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };
constexpr wchar_t kDshowPrefix[] = L"dshow:";
constexpr uint32_t kMaxArea = 1920 * 1080;

void FreeMediaTypeContent(AM_MEDIA_TYPE& mt)
{
    if (mt.cbFormat && mt.pbFormat)
        CoTaskMemFree(mt.pbFormat);
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void DeleteMediaTypePtr(AM_MEDIA_TYPE* mt)
{
    if (mt) {
        FreeMediaTypeContent(*mt);
        CoTaskMemFree(mt);
    }
}

std::wstring ReadBagString(IPropertyBag* bag, const wchar_t* name)
{
    VARIANT v;
    VariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(bag->Read(name, &v, nullptr)) && v.vt == VT_BSTR && v.bstrVal)
        out = v.bstrVal;
    VariantClear(&v);
    return out;
}

struct DshowDevice {
    std::wstring key;  // DevicePath, если есть, иначе FriendlyName
    std::wstring name;
    winrt::com_ptr<IMoniker> moniker;
};

std::vector<DshowDevice> ListDshow()
{
    std::vector<DshowDevice> out;
    winrt::com_ptr<ICreateDevEnum> devEnum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(devEnum.put()))))
        return out;
    winrt::com_ptr<IEnumMoniker> en;
    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, en.put(), 0) != S_OK || !en)
        return out;
    for (;;) {
        winrt::com_ptr<IMoniker> mon;
        if (en->Next(1, mon.put(), nullptr) != S_OK)
            break;
        winrt::com_ptr<IPropertyBag> bag;
        if (FAILED(mon->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(bag.put()))))
            continue;
        DshowDevice d;
        d.name = ReadBagString(bag.get(), L"FriendlyName");
        const std::wstring path = ReadBagString(bag.get(), L"DevicePath");
        d.key = path.empty() ? d.name : path;
        d.moniker = mon;
        if (!d.key.empty())
            out.push_back(std::move(d));
    }
    return out;
}

struct MfDevice {
    std::wstring link;
    std::wstring name;
};

std::vector<MfDevice> ListMf()
{
    std::vector<MfDevice> out;
    winrt::com_ptr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(attr.put(), 1)))
        return out;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attr.get(), &devices, &count)))
        return out;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* name = nullptr;
        WCHAR* link = nullptr;
        UINT32 len = 0;
        devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
        if (link)
            out.push_back({ link, name ? name : L"Камера" });
        CoTaskMemFree(name);
        CoTaskMemFree(link);
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return out;
}

// Колбэк Sample Grabber. Время жизни — у WebcamSource::RunDirectShow, поэтому счётчик ссылок фиктивный.
class GrabberCallback final : public ISampleGrabberCB {
public:
    std::function<void(BYTE*, long)> onBuffer;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB)) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SampleCB(double, IMediaSample*) override { return E_NOTIMPL; }
    STDMETHODIMP BufferCB(double, BYTE* buffer, long len) override
    {
        if (onBuffer && buffer)
            onBuffer(buffer, len);
        return S_OK;
    }
};

} // namespace

std::vector<VideoDevice> ListVideoDevices()
{
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    std::vector<VideoDevice> out;
    for (auto& d : ListMf())
        out.push_back({ d.link, WideToUtf8(d.name), false });
    // Физические камеры видны в обоих API — из DirectShow берём только те, которых нет в MF.
    for (auto& d : ListDshow()) {
        const std::string name = WideToUtf8(d.name);
        const bool dup = std::any_of(out.begin(), out.end(), [&](const VideoDevice& v) { return v.name == name; });
        if (!dup)
            out.push_back({ kDshowPrefix + d.key, name, true });
    }
    MFShutdown();
    return out;
}

WebcamSource::WebcamSource(std::wstring deviceId) : deviceId_(std::move(deviceId))
{
    wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread(&WebcamSource::Thread, this);
}

WebcamSource::~WebcamSource()
{
    quit_ = true;
    if (wake_)
        SetEvent(wake_);
    {
        std::lock_guard lock(mtx_);
        if (shutdownTarget_)
            shutdownTarget_->Shutdown();
    }
    thread_.join();
    if (wake_)
        CloseHandle(wake_);
}

bool WebcamSource::CopyIfNewer(uint64_t knownSeq, WebcamFrame& out) const
{
    // Не ждём поток камеры: занято — на этом кадре берётся прошлый кадр камеры.
    std::unique_lock lock(mtx_, std::try_to_lock);
    if (!lock.owns_lock() || frame_.seq == 0 || frame_.seq == knownSeq)
        return false;
    out.bgra.assign(frame_.bgra.begin(), frame_.bgra.end());
    out.width = frame_.width;
    out.height = frame_.height;
    out.alpha = frame_.alpha;
    out.seq = frame_.seq;
    return true;
}

std::string WebcamSource::Status() const
{
    std::lock_guard lock(mtx_);
    return status_;
}

void WebcamSource::SetStatus(std::string s, bool failed)
{
    if (failed)
        LogWarn("Веб-камера: %s", s.c_str());
    else
        LogInfo("Веб-камера: %s", s.c_str());
    failed_ = failed;
    std::lock_guard lock(mtx_);
    status_ = std::move(s);
}

void WebcamSource::Publish(const uint8_t* data, long pitch, uint32_t w, uint32_t h, bool alpha)
{
    if (!data || w == 0 || h == 0)
        return;
    const size_t row = static_cast<size_t>(w) * 4;
    scratch_.resize(row * h);
    uint8_t anyAlpha = 0;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = data + static_cast<ptrdiff_t>(pitch) * y;
        uint8_t* dst = scratch_.data() + row * y;
        std::memcpy(dst, src, row);
        if (alpha)
            for (size_t i = 3; i < row; i += 4)
                anyAlpha |= dst[i];
    }
    // RGB32 несёт мусор в альфе; ARGB32 с нулевой альфой во всём кадре — тоже считаем непрозрачным,
    // иначе камера просто пропадёт из записи.
    const bool realAlpha = alpha && anyAlpha != 0;
    if (!realAlpha)
        for (size_t i = 3; i < scratch_.size(); i += 4)
            scratch_[i] = 0xFF;

    std::lock_guard lock(mtx_);
    frame_.bgra.swap(scratch_);
    frame_.width = w;
    frame_.height = h;
    frame_.alpha = realAlpha;
    ++frame_.seq;
}

void WebcamSource::Thread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    try {
        std::wstring id = deviceId_;
        if (id.empty()) {
            const auto list = ListVideoDevices();
            if (list.empty())
                throw std::runtime_error("камеры не найдены");
            id = list.front().id;
        }
        if (id.rfind(kDshowPrefix, 0) == 0)
            RunDirectShow(id.substr(std::size(kDshowPrefix) - 1));
        else
            RunMediaFoundation(id);
    } catch (const std::exception& e) {
        SetStatus(e.what(), true);
    } catch (const winrt::hresult_error& e) {
        SetStatus(std::format("ошибка 0x{:08X}", static_cast<unsigned>(e.code())), true);
    }
    MFShutdown();
    CoUninitialize();
}

void WebcamSource::RunMediaFoundation(const std::wstring& link)
{
    std::wstring name = L"Камера";
    for (const auto& d : ListMf())
        if (d.link == link)
            name = d.name;

    winrt::com_ptr<IMFAttributes> attr;
    ThrowIfFailed(MFCreateAttributes(attr.put(), 2), "MFCreateAttributes");
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
    winrt::com_ptr<IMFMediaSource> source;
    const HRESULT hrOpen = MFCreateDeviceSource(attr.get(), source.put());
    if (FAILED(hrOpen))
        throw std::runtime_error(std::format("«{}» не открылась (0x{:08X}) — отключена или занята",
                                             WideToUtf8(name), static_cast<unsigned>(hrOpen)));
    {
        std::lock_guard lock(mtx_);
        shutdownTarget_ = source.get();
    }
    const auto unregister = [&] {
        std::lock_guard lock(mtx_);
        shutdownTarget_ = nullptr;
    };

    try {
        winrt::com_ptr<IMFAttributes> ra;
        ThrowIfFailed(MFCreateAttributes(ra.put(), 1), "MFCreateAttributes(reader)");
        // Декодер MJPG и конвертер YUV → RGB32 вставляет сам Source Reader.
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        winrt::com_ptr<IMFSourceReader> reader;
        ThrowIfFailed(MFCreateSourceReaderFromMediaSource(source.get(), ra.get(), reader.put()),
                      "MFCreateSourceReaderFromMediaSource");
        constexpr DWORD kStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

        // Родной формат: самый большой до 1080p, при равенстве — больше fps; ARGB32 — плюс.
        winrt::com_ptr<IMFMediaType> best;
        double bestScore = -1e30;
        bool bestAlpha = false;
        for (DWORD i = 0;; ++i) {
            winrt::com_ptr<IMFMediaType> t;
            if (FAILED(reader->GetNativeMediaType(kStream, i, t.put())))
                break;
            UINT32 w = 0, h = 0, num = 0, den = 0;
            MFGetAttributeSize(t.get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(t.get(), MF_MT_FRAME_RATE, &num, &den);
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            const double area = static_cast<double>(w) * h;
            const double fps = den ? static_cast<double>(num) / den : 0.0;
            const bool alpha = sub == MFVideoFormat_ARGB32;
            const double score = area <= kMaxArea ? area * 1000.0 + std::min(fps, 60.0) * 10.0 + (alpha ? 1 : 0) : -area;
            if (score > bestScore) {
                bestScore = score;
                best = t;
                bestAlpha = alpha;
            }
        }
        if (best)
            reader->SetCurrentMediaType(kStream, nullptr, best.get());

        auto setOutput = [&](const GUID& sub) {
            winrt::com_ptr<IMFMediaType> out;
            ThrowIfFailed(MFCreateMediaType(out.put()), "MFCreateMediaType");
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID(MF_MT_SUBTYPE, sub);
            return SUCCEEDED(reader->SetCurrentMediaType(kStream, nullptr, out.get()));
        };
        bool alpha = bestAlpha && setOutput(MFVideoFormat_ARGB32);
        if (!alpha && !setOutput(MFVideoFormat_RGB32))
            throw std::runtime_error(std::format("«{}»: нет преобразования в RGB32", WideToUtf8(name)));

        UINT32 w = 0, h = 0;
        LONG stride = 0;
        const auto readFormat = [&] {
            winrt::com_ptr<IMFMediaType> cur;
            ThrowIfFailed(reader->GetCurrentMediaType(kStream, cur.put()), "GetCurrentMediaType");
            MFGetAttributeSize(cur.get(), MF_MT_FRAME_SIZE, &w, &h);
            UINT32 s = 0;
            stride = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? static_cast<LONG>(s)
                                                                        : static_cast<LONG>(w * 4);
        };
        readFormat();
        SetStatus(std::format("{} {}×{}", WideToUtf8(name), w, h), false);

        while (!quit_) {
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            winrt::com_ptr<IMFSample> sample;
            const HRESULT hr = reader->ReadSample(kStream, 0, &stream, &flags, &ts, sample.put());
            if (quit_)
                break;
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                SetStatus(std::format("«{}» остановилась (0x{:08X})", WideToUtf8(name), static_cast<unsigned>(hr)), true);
                break;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                readFormat();
            if (!sample)
                continue;
            winrt::com_ptr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(buf.put())))
                continue;
            if (auto buf2d = buf.try_as<IMF2DBuffer>()) {
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(buf2d->Lock2D(&scan0, &pitch))) {
                    Publish(scan0, pitch, w, h, alpha);
                    buf2d->Unlock2D();
                }
            } else {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                    const DWORD absStride = static_cast<DWORD>(std::abs(stride));
                    if (absStride >= w * 4 && len >= absStride * h) {
                        const BYTE* top = stride < 0 ? data + absStride * (h - 1) : data;
                        Publish(top, stride, w, h, alpha);
                    }
                    buf->Unlock();
                }
            }
        }
    } catch (...) {
        unregister();
        source->Shutdown();
        throw;
    }
    unregister();
    source->Shutdown();
}

void WebcamSource::RunDirectShow(const std::wstring& key)
{
    winrt::com_ptr<IBaseFilter> camera;
    std::wstring name = key;
    for (auto& d : ListDshow()) {
        if (d.key == key) {
            name = d.name;
            ThrowIfFailed(d.moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(camera.put())), "BindToObject(камера)");
            break;
        }
    }
    if (!camera)
        throw std::runtime_error("камера DirectShow не найдена — отключена?");

    winrt::com_ptr<IGraphBuilder> graph;
    ThrowIfFailed(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(graph.put())),
                  "FilterGraph");
    winrt::com_ptr<ICaptureGraphBuilder2> builder;
    ThrowIfFailed(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(builder.put())),
                  "CaptureGraphBuilder2");
    builder->SetFiltergraph(graph.get());
    ThrowIfFailed(graph->AddFilter(camera.get(), L"Camera"), "AddFilter(камера)");

    // Формат камеры: самый большой до 1080p.
    winrt::com_ptr<IAMStreamConfig> streamConfig;
    if (SUCCEEDED(builder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, camera.get(), IID_IAMStreamConfig,
                                         streamConfig.put_void()))) {
        int count = 0, size = 0;
        if (SUCCEEDED(streamConfig->GetNumberOfCapabilities(&count, &size)) && size == sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
            int bestIndex = -1;
            double bestScore = -1e30;
            for (int i = 0; i < count; ++i) {
                AM_MEDIA_TYPE* mt = nullptr;
                VIDEO_STREAM_CONFIG_CAPS caps{};
                if (FAILED(streamConfig->GetStreamCaps(i, &mt, reinterpret_cast<BYTE*>(&caps))) || !mt)
                    continue;
                if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER) && mt->pbFormat) {
                    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
                    const double area = static_cast<double>(vih->bmiHeader.biWidth) * std::abs(vih->bmiHeader.biHeight);
                    const double score = area <= kMaxArea ? area : -area;
                    if (score > bestScore) {
                        bestScore = score;
                        bestIndex = i;
                    }
                }
                DeleteMediaTypePtr(mt);
            }
            if (bestIndex >= 0) {
                AM_MEDIA_TYPE* mt = nullptr;
                VIDEO_STREAM_CONFIG_CAPS caps{};
                if (SUCCEEDED(streamConfig->GetStreamCaps(bestIndex, &mt, reinterpret_cast<BYTE*>(&caps))) && mt) {
                    streamConfig->SetFormat(mt);
                    DeleteMediaTypePtr(mt);
                }
            }
        }
    }

    winrt::com_ptr<IBaseFilter> grabberFilter;
    ThrowIfFailed(CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(grabberFilter.put())),
                  "SampleGrabber (qedit.dll)");
    const auto grabber = grabberFilter.as<ISampleGrabber>();
    AM_MEDIA_TYPE want{};
    want.majortype = MEDIATYPE_Video;
    want.subtype = MEDIASUBTYPE_RGB32;
    ThrowIfFailed(grabber->SetMediaType(&want), "SampleGrabber::SetMediaType");
    ThrowIfFailed(graph->AddFilter(grabberFilter.get(), L"Grabber"), "AddFilter(grabber)");

    winrt::com_ptr<IBaseFilter> nullRenderer;
    ThrowIfFailed(CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(nullRenderer.put())),
                  "NullRenderer");
    ThrowIfFailed(graph->AddFilter(nullRenderer.get(), L"Null"), "AddFilter(null)");

    HRESULT hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, camera.get(), grabberFilter.get(),
                                       nullRenderer.get());
    if (FAILED(hr))
        hr = builder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, camera.get(), grabberFilter.get(),
                                   nullRenderer.get());
    ThrowIfFailed(hr, "RenderStream(камера → RGB32)");

    AM_MEDIA_TYPE got{};
    ThrowIfFailed(grabber->GetConnectedMediaType(&got), "GetConnectedMediaType");
    uint32_t w = 0, h = 0;
    bool bottomUp = true;
    if (got.formattype == FORMAT_VideoInfo && got.cbFormat >= sizeof(VIDEOINFOHEADER) && got.pbFormat) {
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(got.pbFormat);
        w = static_cast<uint32_t>(vih->bmiHeader.biWidth);
        h = static_cast<uint32_t>(std::abs(vih->bmiHeader.biHeight));
        bottomUp = vih->bmiHeader.biHeight > 0;
    }
    FreeMediaTypeContent(got);
    if (w == 0 || h == 0)
        throw std::runtime_error("камера DirectShow: неизвестный размер кадра");

    GrabberCallback cb;
    const long row = static_cast<long>(w) * 4;
    cb.onBuffer = [&](BYTE* data, long len) {
        if (len < row * static_cast<long>(h))
            return;
        if (bottomUp)
            Publish(data + static_cast<size_t>(row) * (h - 1), -row, w, h, false);
        else
            Publish(data, row, w, h, false);
    };
    grabber->SetBufferSamples(FALSE);
    grabber->SetOneShot(FALSE);
    ThrowIfFailed(grabber->SetCallback(&cb, 1 /* BufferCB */), "SampleGrabber::SetCallback");

    // Без опорных часов: кадры отдаются сразу, как пришли.
    if (auto mf = graph.try_as<IMediaFilter>())
        mf->SetSyncSource(nullptr);

    const auto control = graph.as<IMediaControl>();
    const auto events = graph.try_as<IMediaEventEx>();
    ThrowIfFailed(control->Run(), "IMediaControl::Run");
    SetStatus(std::format("{} {}×{} (DirectShow)", WideToUtf8(name), w, h), false);

    while (!quit_) {
        WaitForSingleObject(wake_, 200);
        if (!events)
            continue;
        long code = 0;
        LONG_PTR p1 = 0, p2 = 0;
        while (events->GetEvent(&code, &p1, &p2, 0) == S_OK) {
            events->FreeEventParams(code, p1, p2);
            if (code == EC_DEVICE_LOST || code == EC_ERRORABORT) {
                SetStatus(std::format("«{}» отключилась", WideToUtf8(name)), true);
                quit_ = true;
            }
        }
    }
    control->Stop();
    grabber->SetCallback(nullptr, 1);
}
