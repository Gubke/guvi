#pragma once

#include "capture/webcam_layout.h"

#include <unknwn.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <memory>
#include <vector>

// Картинка виджета эфира. Пиксели не трогать после публикации: их читает и окно, и GPU.
struct OverlaySprite {
    uint64_t seq = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    WebcamLayout layout{};
    std::shared_ptr<const std::vector<uint8_t>> bgra;
};

// Несколько картинок поверх кадра эфира. Рисуется на GPU в текстуру второго кодировщика,
// в запись на диск не попадает. Загрузка в текстуру — только когда сменился seq.
class OverlayCompositor {
public:
    static constexpr int kMax = 4;

    OverlayCompositor(ID3D11Device* device, ID3D11Texture2D* target);
    void Draw(ID3D11DeviceContext* ctx, const std::vector<OverlaySprite>& sprites);

private:
    struct Slot {
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        uint32_t w = 0, h = 0;
        uint64_t seq = 0;
    };

    void DrawOne(ID3D11DeviceContext* ctx, Slot& slot, const OverlaySprite& sprite);

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11RenderTargetView> rtv_;
    winrt::com_ptr<ID3D11VertexShader> vs_;
    winrt::com_ptr<ID3D11PixelShader> ps_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11BlendState> blend_;
    uint32_t targetW_ = 0, targetH_ = 0;
    Slot slots_[kMax]{};
};
