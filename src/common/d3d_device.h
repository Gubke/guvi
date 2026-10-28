#pragma once

#include <unknwn.h>
#include <d3d11_4.h>
#include <winrt/base.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <string>

enum class GpuVendor { Unknown, Nvidia, Amd, Intel };

struct D3DContext {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{ nullptr };
    GpuVendor vendor = GpuVendor::Unknown;
    std::string adapterName;
    LUID adapterLuid{};
};

const char* VendorName(GpuVendor v);

// Создаёт D3D11-устройство. Порядок адаптеров: NVIDIA → AMD → Intel → по умолчанию,
// чтобы кодировщик (NVENC / AMF / Quick Sync) жил на той же видеокарте, что и текстуры.
// Контекст защищён ID3D11Multithread: им пользуются поток захвата и поток кодирования.
// prefer — явно выбранный производитель (кодировщик задан вручную), Unknown — по порядку выше.
D3DContext CreateD3DContext(GpuVendor prefer = GpuVendor::Unknown);
