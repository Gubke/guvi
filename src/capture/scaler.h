#pragma once

#include <unknwn.h>
#include <d3d11.h>
#include <winrt/base.h>

// Масштабирование кадра на GPU (полноэкранный треугольник + билинейная выборка) с сохранением
// пропорций: лишнее заливается чёрным. Нужно, когда выбранное разрешение меньше источника.
class Scaler {
public:
    // src — текстура с SHADER_RESOURCE, dst — с RENDER_TARGET (вход кодировщика).
    Scaler(ID3D11Device* device, ID3D11Texture2D* src, ID3D11Texture2D* dst);

    void Draw(ID3D11DeviceContext* ctx);

private:
    winrt::com_ptr<ID3D11VertexShader> vs_;
    winrt::com_ptr<ID3D11PixelShader> ps_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11ShaderResourceView> srv_;
    winrt::com_ptr<ID3D11RenderTargetView> rtv_;
    D3D11_VIEWPORT viewport_{};
};
