#include "common/d3d_device.h"
#include "common/hr.h"
#include "common/log.h"

#include <dxgi1_6.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <vector>

namespace {

GpuVendor VendorFromId(UINT id)
{
    switch (id) {
    case 0x10DE: return GpuVendor::Nvidia;
    case 0x1002: case 0x1022: return GpuVendor::Amd;
    case 0x8086: return GpuVendor::Intel;
    default: return GpuVendor::Unknown;
    }
}

int VendorRank(GpuVendor v)
{
    switch (v) {
    case GpuVendor::Nvidia: return 0;
    case GpuVendor::Amd: return 1;
    case GpuVendor::Intel: return 2;
    default: return 3;
    }
}

struct AdapterInfo {
    winrt::com_ptr<IDXGIAdapter1> adapter;
    GpuVendor vendor = GpuVendor::Unknown;
    std::string name;
    LUID luid{};
};

AdapterInfo PickAdapter(GpuVendor prefer)
{
    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
        return {};

    std::vector<AdapterInfo> list;
    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            AdapterInfo a;
            a.adapter = adapter;
            a.vendor = VendorFromId(desc.VendorId);
            a.name = WideToUtf8(desc.Description);
            a.luid = desc.AdapterLuid;
            list.push_back(std::move(a));
        }
        adapter = nullptr;
    }
    if (list.empty())
        return {};

    if (prefer != GpuVendor::Unknown) {
        for (auto& a : list)
            if (a.vendor == prefer)
                return a;
    }
    AdapterInfo* best = &list[0];
    for (auto& a : list)
        if (VendorRank(a.vendor) < VendorRank(best->vendor))
            best = &a;
    return *best;
}

} // namespace

const char* VendorName(GpuVendor v)
{
    switch (v) {
    case GpuVendor::Nvidia: return "NVIDIA";
    case GpuVendor::Amd: return "AMD";
    case GpuVendor::Intel: return "Intel";
    default: return "GPU";
    }
}

D3DContext CreateD3DContext(GpuVendor prefer)
{
    D3DContext ctx;

    const AdapterInfo picked = PickAdapter(prefer);
    if (picked.adapter) {
        ctx.vendor = picked.vendor;
        ctx.adapterName = picked.name;
        ctx.adapterLuid = picked.luid;
        LogInfo("GPU: %s", picked.name.c_str());
    } else {
        LogWarn("Аппаратный адаптер не найден, используется адаптер по умолчанию");
    }

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    // VIDEO_SUPPORT нужен MF-кодировщику (ID3D11VideoProcessor для BGRA → NV12).
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    auto create = [&](UINT f) {
        return D3D11CreateDevice(picked.adapter.get(),
                                 picked.adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, f,
                                 levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, ctx.device.put(), nullptr,
                                 ctx.context.put());
    };
    HRESULT hr = create(flags);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG))
        hr = create(flags & ~D3D11_CREATE_DEVICE_DEBUG);  // Debug layer может быть не установлен
    if (FAILED(hr))
        hr = create(flags & ~(D3D11_CREATE_DEVICE_DEBUG | D3D11_CREATE_DEVICE_VIDEO_SUPPORT));
    ThrowIfFailed(hr, "D3D11CreateDevice");

    if (auto mt = ctx.device.try_as<ID3D11Multithread>())
        mt->SetMultithreadProtected(TRUE);

    const auto dxgiDevice = ctx.device.as<IDXGIDevice>();
    // Игра грузит GPU на 100 % — без приоритета наши копии/шейдеры ждут в очереди за её кадрами.
    // Положительный приоритет может требовать прав; отказ не критичен.
    if (FAILED(dxgiDevice->SetGPUThreadPriority(7)))
        dxgiDevice->SetGPUThreadPriority(0);
    winrt::com_ptr<::IInspectable> inspectable;
    ThrowIfFailed(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()),
                  "CreateDirect3D11DeviceFromDXGIDevice");
    ctx.winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    return ctx;
}
