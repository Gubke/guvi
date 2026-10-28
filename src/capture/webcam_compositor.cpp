#include "capture/webcam_compositor.h"
#include "common/hr.h"

#include <d3dcompiler.h>

#include <cstring>

namespace {

constexpr char kShader[] = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID)
{
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_main(V i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

winrt::com_ptr<ID3DBlob> Compile(const char* entry, const char* target)
{
    winrt::com_ptr<ID3DBlob> code;
    winrt::com_ptr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kShader, std::strlen(kShader), "webcam", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
    if (FAILED(hr)) {
        const std::string msg = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                                     errors->GetBufferSize())
                                       : std::string("?");
        throw std::runtime_error("Шейдер веб-камеры: " + msg);
    }
    return code;
}

} // namespace

WebcamCompositor::WebcamCompositor(ID3D11Device* device, ID3D11Texture2D* target)
{
    device_.copy_from(device);
    D3D11_TEXTURE2D_DESC td{};
    target->GetDesc(&td);
    targetW_ = td.Width;
    targetH_ = td.Height;
    ThrowIfFailed(device->CreateRenderTargetView(target, nullptr, rtv_.put()), "CreateRenderTargetView(webcam)");

    const auto vs = Compile("vs_main", "vs_4_0");
    const auto ps = Compile("ps_main", "ps_4_0");
    ThrowIfFailed(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vs_.put()),
                  "CreateVertexShader(webcam)");
    ThrowIfFailed(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, ps_.put()),
                  "CreatePixelShader(webcam)");

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device->CreateSamplerState(&sd, sampler_.put()), "CreateSamplerState(webcam)");

    // Обычная (не premultiplied) альфа: камеры с прозрачностью отдают именно её.
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ThrowIfFailed(device->CreateBlendState(&bd, blend_.put()), "CreateBlendState(webcam)");
}

void WebcamCompositor::Update(ID3D11DeviceContext* ctx, const WebcamSource& source)
{
    if (!source.CopyIfNewer(seq_, frame_))
        return;
    seq_ = frame_.seq;
    if (!tex_ || camW_ != frame_.width || camH_ != frame_.height) {
        tex_ = nullptr;
        srv_ = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = frame_.width;
        desc.Height = frame_.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, tex_.put())) ||
            FAILED(device_->CreateShaderResourceView(tex_.get(), nullptr, srv_.put()))) {
            tex_ = nullptr;
            srv_ = nullptr;
            hasFrame_ = false;
            return;
        }
        camW_ = frame_.width;
        camH_ = frame_.height;
    }
    ctx->UpdateSubresource(tex_.get(), 0, nullptr, frame_.bgra.data(), frame_.width * 4, 0);
    hasFrame_ = true;
}

void WebcamCompositor::Draw(ID3D11DeviceContext* ctx, const WebcamLayout& layout)
{
    if (!hasFrame_ || !srv_)
        return;
    const WebcamRect r = PlaceWebcam(static_cast<float>(targetW_), static_cast<float>(targetH_),
                                     static_cast<float>(camW_), static_cast<float>(camH_), layout);
    if (r.w < 1 || r.h < 1)
        return;

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = r.x;
    vp.TopLeftY = r.y;
    vp.Width = r.w;
    vp.Height = r.h;
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtv = rtv_.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const float factor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(blend_.get(), factor, 0xFFFFFFFF);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.get(), nullptr, 0);
    ctx->PSSetShader(ps_.get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = srv_.get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = sampler_.get();
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetBlendState(nullptr, factor, 0xFFFFFFFF);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}
