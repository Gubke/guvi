#include "app/screenshot.h"
#include "common/hr.h"

#include <objbase.h>
#include <wincodec.h>

#include <cstring>

CpuImage ScreenshotReader::Read(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* src)
{
    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC have{};
    if (staging_)
        staging_->GetDesc(&have);
    if (!staging_ || have.Width != desc.Width || have.Height != desc.Height) {
        D3D11_TEXTURE2D_DESC sd = desc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        staging_ = nullptr;
        ThrowIfFailed(device->CreateTexture2D(&sd, nullptr, staging_.put()), "CreateTexture2D(staging)");
    }

    context->CopyResource(staging_.get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    ThrowIfFailed(context->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped), "Map(staging)");

    CpuImage img;
    img.width = desc.Width;
    img.height = desc.Height;
    img.bgra.resize(static_cast<size_t>(desc.Width) * desc.Height * 4);
    const size_t rowBytes = static_cast<size_t>(desc.Width) * 4;
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(&img.bgra[y * rowBytes], static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, rowBytes);
    context->Unmap(staging_.get(), 0);

    // WGC не гарантирует альфу (у окон бывает 0) — в PNG она должна быть непрозрачной.
    for (size_t i = 3; i < img.bgra.size(); i += 4)
        img.bgra[i] = 0xFF;
    return img;
}

void SavePng(const CpuImage& img, const std::wstring& path)
{
    winrt::com_ptr<IWICImagingFactory> factory;
    ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.put())),
                  "WICImagingFactory");

    winrt::com_ptr<IWICStream> stream;
    ThrowIfFailed(factory->CreateStream(stream.put()), "IWICImagingFactory::CreateStream");
    ThrowIfFailed(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "IWICStream::InitializeFromFilename");

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    ThrowIfFailed(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()), "CreateEncoder(PNG)");
    ThrowIfFailed(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache), "IWICBitmapEncoder::Initialize");

    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    ThrowIfFailed(encoder->CreateNewFrame(frame.put(), nullptr), "CreateNewFrame");
    ThrowIfFailed(frame->Initialize(nullptr), "IWICBitmapFrameEncode::Initialize");
    ThrowIfFailed(frame->SetSize(img.width, img.height), "SetSize");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ThrowIfFailed(frame->SetPixelFormat(&fmt), "SetPixelFormat");
    if (fmt != GUID_WICPixelFormat32bppBGRA)
        throw std::runtime_error("PNG-кодер WIC не принял BGRA");

    const UINT stride = img.width * 4;
    ThrowIfFailed(frame->WritePixels(img.height, stride, static_cast<UINT>(img.bgra.size()),
                                     const_cast<BYTE*>(img.bgra.data())),
                  "WritePixels");
    ThrowIfFailed(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    ThrowIfFailed(encoder->Commit(), "IWICBitmapEncoder::Commit");
}
