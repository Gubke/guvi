#pragma once

#include "capture/webcam_layout.h"
#include "capture/webcam_source.h"

#include <unknwn.h>
#include <d3d11.h>
#include <winrt/base.h>

// Накладывает кадр веб-камеры на кадр, уходящий в кодировщик (на GPU, с альфа-смешиванием),
// поэтому камера есть в записи, повторах и скриншотах.
class WebcamCompositor {
public:
    WebcamCompositor(ID3D11Device* device, ID3D11Texture2D* target);

    // Новый кадр камеры, если он появился.
    void Update(ID3D11DeviceContext* ctx, const WebcamSource& source);
    void Reset() { seq_ = 0; hasFrame_ = false; }
    void Draw(ID3D11DeviceContext* ctx, const WebcamLayout& layout);

private:
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11RenderTargetView> rtv_;
    winrt::com_ptr<ID3D11VertexShader> vs_;
    winrt::com_ptr<ID3D11PixelShader> ps_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11BlendState> blend_;
    winrt::com_ptr<ID3D11Texture2D> tex_;
    winrt::com_ptr<ID3D11ShaderResourceView> srv_;
    uint32_t targetW_ = 0, targetH_ = 0;
    uint32_t camW_ = 0, camH_ = 0;
    uint64_t seq_ = 0;
    bool hasFrame_ = false;
    WebcamFrame frame_;
};
