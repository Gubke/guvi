#pragma once

#include <unknwn.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>
#include <vector>

struct CpuImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;  // stride = width * 4
};

// Единственное место с чтением кадра в CPU: только по F10, через staging-текстуру.
class ScreenshotReader {
public:
    CpuImage Read(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* src);

private:
    winrt::com_ptr<ID3D11Texture2D> staging_;
};

// WIC PNG; вызывать из потока с инициализированным COM.
void SavePng(const CpuImage& img, const std::wstring& path);
