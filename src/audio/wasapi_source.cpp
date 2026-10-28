#include "audio/wasapi_source.h"
#include "common/hr.h"

#include <objbase.h>

#include <initguid.h>  // определение PKEY_Device_FriendlyName (библиотеки с ним нет)
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>

namespace {

constexpr REFERENCE_TIME kBufferHns = 2'000'000;  // 200 мс: источник опрашивается, а не по событию

std::string DeviceName(IMMDevice* device)
{
    winrt::com_ptr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, props.put())))
        return "?";
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name = "?";
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
        name = WideToUtf8(v.pwszVal);
    PropVariantClear(&v);
    return name;
}

winrt::com_ptr<IMMDeviceEnumerator> CreateEnumerator()
{
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    ThrowIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(enumerator.put())),
                  "MMDeviceEnumerator");
    return enumerator;
}

std::wstring DeviceId(IMMDevice* device)
{
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(device->GetId(&id)) && id)
        out = id;
    CoTaskMemFree(id);
    return out;
}

} // namespace

std::vector<AudioDevice> ListAudioDevices(bool capture)
{
    std::vector<AudioDevice> out;
    try {
        const auto enumerator = CreateEnumerator();
        const EDataFlow flow = capture ? eCapture : eRender;

        std::wstring defaultId;
        winrt::com_ptr<IMMDevice> def;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, def.put())))
            defaultId = DeviceId(def.get());

        winrt::com_ptr<IMMDeviceCollection> devices;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, devices.put())))
            return out;
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            winrt::com_ptr<IMMDevice> d;
            if (FAILED(devices->Item(i, d.put())))
                continue;
            AudioDevice e;
            e.id = DeviceId(d.get());
            e.name = DeviceName(d.get());
            e.isDefault = !defaultId.empty() && e.id == defaultId;
            out.push_back(std::move(e));
        }
    } catch (const std::exception&) {
    }
    return out;
}

WasapiSource::WasapiSource(Kind kind, const std::wstring& deviceId)
    : kind_(kind)
{
    const bool loopback = kind == Kind::SystemLoopback;
    const auto enumerator = CreateEnumerator();

    winrt::com_ptr<IMMDevice> device;
    if (!deviceId.empty()) {
        DWORD state = 0;
        if (FAILED(enumerator->GetDevice(deviceId.c_str(), device.put())) ||
            FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) {
            device = nullptr;
            // Устройство отключили — пишем с устройства по умолчанию, а не падаем.
        }
    }
    if (!device)
        ThrowIfFailed(enumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, device.put()),
                      loopback ? "Нет устройства воспроизведения по умолчанию" : "Нет микрофона по умолчанию");
    name_ = DeviceName(device.get());

    ThrowIfFailed(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.put_void()),
                  "IMMDevice::Activate(IAudioClient)");

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = kChannels;
    fmt.nSamplesPerSec = kSampleRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (loopback)
        flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    ThrowIfFailed(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferHns, 0, &fmt, nullptr),
                  "IAudioClient::Initialize");
    ThrowIfFailed(client_->GetService(IID_PPV_ARGS(capture_.put())), "IAudioClient::GetService(capture)");
}

WasapiSource::~WasapiSource()
{
    Stop();
}

void WasapiSource::Start()
{
    if (!started_) {
        ThrowIfFailed(client_->Start(), "IAudioClient::Start");
        started_ = true;
    }
}

void WasapiSource::Stop()
{
    if (started_) {
        client_->Stop();
        started_ = false;
    }
}

void WasapiSource::Drain(const PacketFn& fn)
{
    UINT32 next = 0;
    while (SUCCEEDED(capture_->GetNextPacketSize(&next)) && next > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 devicePos = 0;
        UINT64 qpcHns = 0;
        const HRESULT hr = capture_->GetBuffer(&data, &frames, &flags, &devicePos, &qpcHns);
        if (hr == AUDCLNT_S_BUFFER_EMPTY)
            return;
        ThrowIfFailed(hr, "IAudioCaptureClient::GetBuffer");

        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const int64_t ts = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ? kNoTimestamp : static_cast<int64_t>(qpcHns);
        fn(ts, silent ? nullptr : reinterpret_cast<const float*>(data), frames);

        ThrowIfFailed(capture_->ReleaseBuffer(frames), "IAudioCaptureClient::ReleaseBuffer");
    }
}
