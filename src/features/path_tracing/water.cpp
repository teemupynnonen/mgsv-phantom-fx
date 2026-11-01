// Rivers and other liquids on Direct3D 11; water.hpp describes them. What they share with glass and mirrors - the surface
// targets, the trace and the composite - is forward_surfaces'.
#include "features/path_tracing/water.hpp"

#include "d3d11_helpers.hpp"
#include "features/path_tracing/forward_surfaces.hpp"
#include "features/path_tracing/path_tracing.hpp"
#include "hresult.hpp"
#include "log.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"

#include <cstdint>
#include <exception>
#include <string>

namespace phantom_fx::water
{
namespace
{
using Microsoft::WRL::ComPtr;

// Where all twelve river shaders read their reflection and their shadow.
constexpr UINT CubeSlot = 2, ShadowSlot = 14, ShadowSamplerSlot = 15;

// Made at attach and only read until detach. Together, a shadow map whose comparison always passes: no map shadow.
ComPtr<ID3D11ShaderResourceView> unshadowedMap;
ComPtr<ID3D11SamplerState> alwaysPasses;
}

void attach(ID3D11Device* device) noexcept
{
    try
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const std::uint8_t white = 0xFF;
        const D3D11_SUBRESOURCE_DATA data{&white, 1, 0};
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, &data, texture.GetAddressOf()), "Creating the rivers' empty shadow map");
        check(device->CreateShaderResourceView(texture.Get(), nullptr, unshadowedMap.ReleaseAndGetAddressOf()), "Creating the rivers' empty shadow map");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler, alwaysPasses.ReleaseAndGetAddressOf()), "Creating the rivers' empty shadow map");
    }
    catch (const std::exception& error)
    {
        log(std::string("The rivers keep the game's shadow: ") + error.what());
        unshadowedMap.Reset();
        alwaysPasses.Reset();
    }
}

void detach() noexcept
{
    unshadowedMap.Reset();
    alwaysPasses.Reset();
}

bool drawRiver(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    const auto drawGame = [&] { context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance); };

    try
    {
        ComPtr<ID3D11RenderTargetView> target;
        context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        const auto [width, height] = sizeOf(target.Get());

        const bool isReflecting = path_tracing::isReflecting() && forward_surfaces::accepts(context);
        const bool leavesShadowMap = unshadowedMap && (isReflecting || ray_tracing::sunShadow(width, height));
        if (!isReflecting && !leavesShadowMap)
        {
            drawGame();
            return false;
        }

        logOnce(isReflecting ? "The rivers: the rays take their shadow and their reflection." : "The rivers: the rays take their shadow.");

        ComPtr<ID3D11ShaderResourceView> gameCube, gameShadow;
        ComPtr<ID3D11SamplerState> gameSampler;
        context->PSGetShaderResources(CubeSlot, 1, gameCube.GetAddressOf());
        context->PSGetShaderResources(ShadowSlot, 1, gameShadow.GetAddressOf());
        context->PSGetSamplers(ShadowSamplerSlot, 1, gameSampler.GetAddressOf());

        // A view of nothing reads black: the traced reflection takes the cube map's place, at the composite.
        ID3D11ShaderResourceView* none{};
        if (isReflecting) context->PSSetShaderResources(CubeSlot, 1, &none);
        if (leavesShadowMap)
        {
            context->PSSetShaderResources(ShadowSlot, 1, unshadowedMap.GetAddressOf());
            context->PSSetSamplers(ShadowSamplerSlot, 1, alwaysPasses.GetAddressOf());
        }
        drawGame();

        context->PSSetShaderResources(CubeSlot, 1, gameCube.GetAddressOf());
        context->PSSetShaderResources(ShadowSlot, 1, gameShadow.GetAddressOf());
        context->PSSetSamplers(ShadowSamplerSlot, 1, gameSampler.GetAddressOf());

        return isReflecting;
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("The rivers keep the game's light: ") + error.what());
        drawGame();
        return false;
    }
}
}
