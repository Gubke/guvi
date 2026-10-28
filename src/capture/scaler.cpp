#include "capture/scaler.h"
#include "common/hr.h"

#include <d3dcompiler.h>

#include <algorithm>
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
float4 ps_main(V i) : SV_Target { return float4(tex.Sample(smp, i.uv).rgb, 1); }
)";

winrt::com_ptr<ID3DBlob> Compile(const char* entry, const char* target)
{
    winrt::com_ptr<ID3DBlob> code;
    winrt::com_ptr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kShader, std::strlen(kShader), "scaler", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
    if (FAILED(hr)) {
        const std::string msg = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                                     errors->GetBufferSize())
                                       : std::string("?");
        throw std::runtime_error("Шейдер масштабирования: " + msg);
    }
    return code;
}

} // namespace

Scaler::Scaler(ID3D11Device* device, ID3D11Texture2D* src, ID3D11Texture2D* dst)
{
    const auto vs = Compile("vs_main", "vs_4_0");
    const auto ps = Compile("ps_main", "ps_4_0");
    ThrowIfFailed(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vs_.put()),
                  "CreateVertexShader");
    ThrowIfFailed(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, ps_.put()),
                  "CreatePixelShader");

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device->CreateSamplerState(&sd, sampler_.put()), "CreateSamplerState");
    ThrowIfFailed(device->CreateShaderResourceView(src, nullptr, srv_.put()), "CreateShaderResourceView(scaler)");
    ThrowIfFailed(device->CreateRenderTargetView(dst, nullptr, rtv_.put()), "CreateRenderTargetView(scaler)");

    D3D11_TEXTURE2D_DESC s{}, d{};
    src->GetDesc(&s);
    dst->GetDesc(&d);
    const float scale = std::min(static_cast<float>(d.Width) / static_cast<float>(s.Width),
                                 static_cast<float>(d.Height) / static_cast<float>(s.Height));
    viewport_.Width = static_cast<float>(s.Width) * scale;
    viewport_.Height = static_cast<float>(s.Height) * scale;
    viewport_.TopLeftX = (static_cast<float>(d.Width) - viewport_.Width) * 0.5f;
    viewport_.TopLeftY = (static_cast<float>(d.Height) - viewport_.Height) * 0.5f;
    viewport_.MaxDepth = 1.0f;
}

void Scaler::Draw(ID3D11DeviceContext* ctx)
{
    const float black[4] = { 0, 0, 0, 1 };
    ctx->ClearRenderTargetView(rtv_.get(), black);
    ID3D11RenderTargetView* rtv = rtv_.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &viewport_);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.get(), nullptr, 0);
    ctx->PSSetShader(ps_.get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = srv_.get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = sampler_.get();
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->Draw(3, 0);
    // Отвязать, чтобы следующий CopyResource в источник не конфликтовал с привязкой SRV.
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}
