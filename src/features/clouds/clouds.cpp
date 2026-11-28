// Better clouds: clouds.hpp describes them.
#include "features/clouds/clouds.hpp"

#include "d3d11_helpers.hpp"
#include "features/clouds/cloud_noise.hpp"
#include "features/path_tracing/sky_capture.hpp"
#include "features/volumetric_fog/atmosphere.hpp"
#include "game_shaders.hpp"
#include "log.hpp"
#include "performance.hpp"
#include "phantom_fx_settings.hpp"
#include "weather.hpp"
#include "phantom_shaders/cloud_shadow.hpp"
#include "phantom_shaders/clouds.hpp"
#include "phantom_shaders/clouds_composite.hpp"
#include "phantom_shaders/clouds_resolve.hpp"
#include "phantom_shaders/clouds_vs.hpp"
#if PHANTOM_FX_DEV_TOOLS
#include "hot_shader.hpp"
#endif

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <mutex>
#include <string_view>
#include <chrono>
#include <vector>

namespace phantom_fx::clouds
{
namespace
{
using Microsoft::WRL::ComPtr;

// tpp3dfw_generativeClouds, the game's cloud pixel shader (game_shaders.hpp).
constexpr std::string_view CloudChecksum = "4e081ddecae4b78e7229ae6dec61d2ee";
// tpp3dfw_constant_sky, the painted dome drawn just before them.
constexpr std::string_view DomeChecksum = "1d8d495cfad8edf0a94795d1b83ab365";

std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> pixel;
// The full-screen draw: its vertex shader, and states without a depth test and without culling.
ComPtr<ID3D11VertexShader> screenVertex;
ComPtr<ID3D11DepthStencilState> noDepth;
ComPtr<ID3D11RasterizerState> noCulling;
// The slot the game binds the scene's depth at for its cloud draw, and the one the add-on's 3D noise takes.
constexpr UINT SceneDepthSlot = 13, CloudNoiseSlot = 14;
// A 64-cube of fbm (cloud_noise): the game's own cloud texture has eight slices.
constexpr int NoiseSize = cloud_noise::Size;
ComPtr<ID3D11Texture3D> noiseTexture;
ComPtr<ID3D11ShaderResourceView> noiseView;
// Trilinear and wrapping, over every mip: the game's samplers may stop at the first level. Bound at s12 for the draw.
constexpr UINT NoiseSamplerSlot = 12;
// The physical sky's parameters (atmosphere::skyParametersView), so the clouds have its sun and sky; empty while it is off.
constexpr UINT SkyParametersSlot = 18;
ComPtr<ID3D11SamplerState> noiseSampler;
// The ground's cloud shadow (cloud_shadow.hlsl): the sun's transmittance through the layer, over a square around the eye.
// ShadowSize and the extent match cloud_density.hlsli. Drawn once a frame before the sky; the sky's draw reads it at t16.
constexpr UINT ShadowSize = 1024, CloudShadowSlot = 16, SettingsSlot = 7;
ComPtr<ID3D11PixelShader> shadowPixel;
ComPtr<ID3D11Texture2D> shadowMap;
std::atomic<bool> shadowDrawn{}; // Whether the map holds a frame's shadow yet
ComPtr<ID3D11RenderTargetView> shadowTarget;
ComPtr<ID3D11ShaderResourceView> shadowView;
// cCloudSettings (b7) of clouds.hlsl and cloud_shadow.hlsl: the menu's quality and its tweakables.
ComPtr<ID3D11BlendState> opaqueBlend;
ComPtr<ID3D11Buffer> settingsBuffer;
struct ShaderSettings
{
    float steps, coverage, size, wind, shadow;
    // The distance the clouds travel in this draw, in metres: the weather's wind times the Wind setting, over the time since the
    // last draw at the game clock's pace, so they keep up with fast time and a slider changes their pace, not their place.
    float stepX, stepZ;
    float fog; // 1 the game's fog over the clouds, 0 none
    // Counts the draws: the march's dither turns with it, so the upscaler's history averages the grain at the clouds' edges away.
    float frame;
    float weatherCover; // CoverOf the weather, eased over about half a minute so a change of weather fills or clears the sky
    float padding[2];
};
static_assert(sizeof(ShaderSettings) % 16 == 0);
std::atomic<float> steps{192}, coverage{1}, size{1}, wind{1}, shadow{1}, fog{1};
// The clouds' drift (cloud_wind below): the distance travelled in metres, summed on the GPU from each draw's step.
// A structured buffer of two float4s, [0].xy the distance.
// The weather's wind at the cloud layer in metres a second (the sea's table, as the clouds follow the same wind), and its
// direction: the layer's own is not in the game's constants, only an offset that stops with its clock.
constexpr float CloudWindScale = 2;
constexpr float WindDirectionX = 0.8f, WindDirectionZ = 0.6f;
// The game's clock at its normal pace, in game seconds per real second (measured: 587 in 29.4 s), what the wind's speed is
// for; and the most the drift follows it, in times the real step, so a set clock does not fling the clouds across the sky.
constexpr float NormalClockRate = 20, MostClockSpeedUp = 50, SecondsPerDay = 86400;
constexpr float WindOf(weather::Type type)
{
    switch (type)
    {
    case weather::Type::Cloudy: return 8;
    case weather::Type::Rainy: return 14;
    case weather::Type::Sandstorm: return 10;
    case weather::Type::Foggy: return 4;
    default: return 6;
    }
}
// The weather's cloud cover, 0 to 1: the game's own (its cloud material's coverage) fades to nothing as the sun sets, as its painted
// clouds did, and took the layer with it at dusk. The weather does not change at night, so neither does its cloud.
constexpr float CoverOf(weather::Type type)
{
    switch (type)
    {
    case weather::Type::Cloudy: return 0.85f;
    case weather::Type::Rainy: return 1.0f;
    case weather::Type::Sandstorm: return 0.7f;
    case weather::Type::Foggy: return 0.9f;
    default: return 0.6f;
    }
}
constexpr UINT WindSlot = 17;
ComPtr<ID3D11ComputeShader> windShader;
ComPtr<ID3D11Buffer> windState;
ComPtr<ID3D11UnorderedAccessView> windAccess;
ComPtr<ID3D11ShaderResourceView> windView;
constexpr const char* WindSource = R"(
cbuffer cPSMaterial : register(b4) { float4 m_materials[8]; };
cbuffer cCloudSettings : register(b7) { float g_cloudSteps, g_cloudCoverage, g_cloudSize, g_cloudWind, g_cloudShadow; float2 g_cloudStep; float g_padding; };
RWStructuredBuffer<float4> State : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    State[0].xy += g_cloudStep;
}
)";

// The field the add-on ships: gradient noise, which has no axis to line up along, its lattice bent by a warp. The preview tool
// (tools/noise-preview) bakes the same recipe, so what it shows is what the sky gets.
cloud_noise::Recipe shippingRecipe()
{
    cloud_noise::Recipe recipe;
    recipe.generator = cloud_noise::Generator::Gradient;
    recipe.warp = 0.05f;
    recipe.median = std::nanf("");
    return recipe;
}
#if PHANTOM_FX_DEV_TOOLS
std::unique_ptr<HotShader> hot; // Watches clouds.hlsl; a saved edit replaces `pixel` at the next cloud draw
std::unique_ptr<HotShader> hotShadow; // The same for cloud_shadow.hlsl and `shadowPixel`
#endif
std::atomic<bool> isOn{};
bool hasFailed{};
std::atomic<bool> drawn{}; // The add-on has drawn the clouds over the scene at least once since they were turned on

// The march at half the render size (clouds.hlsl, LowScale): a quarter of the pixels, each for a 2 x 2 block, and then
// clouds_composite.hlsl over the scene at full size. At full size the clouds cost 5 to 9 ms at 1080p.
constexpr UINT LowScale = 2, LowColorSlot = 19, LowDistanceSlot = 20, HistorySlot = 21, ViewHistorySlot = 22;
ComPtr<ID3D11PixelShader> compositePixel;
// The march's history (clouds_resolve.hlsl), two targets drawn in turn, and the view it was drawn from: the march's dither turns
// every frame, and alone each frame flickered in 2 x 2 blocks.
ComPtr<ID3D11PixelShader> resolvePixel;
ComPtr<ID3D11Texture2D> history[2];
ComPtr<ID3D11RenderTargetView> historyTarget[2];
ComPtr<ID3D11ShaderResourceView> historyView[2];
UINT historyIndex{}; // The one drawn last
ComPtr<ID3D11Buffer> viewHistory;
ComPtr<ID3D11UnorderedAccessView> viewHistoryAccess;
ComPtr<ID3D11ShaderResourceView> viewHistoryView;
ComPtr<ID3D11ComputeShader> viewCopyShader;
constexpr const char* ViewCopySource = R"(
cbuffer cPSScene : register(b2) { float4 m_scene[26]; };
RWStructuredBuffer<float4> State : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    State[0] = m_scene[4];
    State[1] = m_scene[5];
    State[2] = m_scene[8];
    State[3] = m_scene[9];
    State[4] = m_scene[10];
    State[5] = float4(1, 0, 0, 0);
}
)";
UINT lowWidth{}, lowHeight{};
ComPtr<ID3D11Texture2D> lowColor, lowDistance;
ComPtr<ID3D11RenderTargetView> lowColorTarget, lowDistanceTarget;
ComPtr<ID3D11ShaderResourceView> lowColorView, lowDistanceView;

void ensurePipeline() noexcept
{
    if (pixel || hasFailed || !device) return;

    try
    {
        // Precompiled by fxc (cmake/phantom_shaders.cmake), not compiled here.
        if (FAILED(device->CreatePixelShader(g_cloudsPixelShader, sizeof(g_cloudsPixelShader), nullptr, pixel.GetAddressOf())))
            throw std::runtime_error("CreatePixelShader failed");
        if (FAILED(device->CreateVertexShader(g_cloudsVertexShader, sizeof(g_cloudsVertexShader), nullptr, screenVertex.GetAddressOf())))
            throw std::runtime_error("CreateVertexShader failed");
        D3D11_DEPTH_STENCIL_DESC depth{};
        check(device->CreateDepthStencilState(&depth, noDepth.GetAddressOf()), "The clouds' depth state");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&raster, noCulling.GetAddressOf()), "The clouds' rasterizer state");

        if (FAILED(device->CreatePixelShader(g_cloudsCompositePixelShader, sizeof(g_cloudsCompositePixelShader), nullptr, compositePixel.GetAddressOf())))
            throw std::runtime_error("CreatePixelShader failed for the composite");
        if (FAILED(device->CreatePixelShader(g_cloudsResolvePixelShader, sizeof(g_cloudsResolvePixelShader), nullptr, resolvePixel.GetAddressOf())))
            throw std::runtime_error("CreatePixelShader failed for the history");
        viewCopyShader = computeShader(device.Get(), ViewCopySource, "cloud-view-history");
        D3D11_BUFFER_DESC viewDesc{};
        viewDesc.ByteWidth = 6 * 16;
        viewDesc.Usage = D3D11_USAGE_DEFAULT;
        viewDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        viewDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        viewDesc.StructureByteStride = 16;
        const float noView[24]{};
        const D3D11_SUBRESOURCE_DATA viewData{noView, 0, 0};
        check(device->CreateBuffer(&viewDesc, &viewData, viewHistory.GetAddressOf()), "The clouds' view history");
        check(device->CreateUnorderedAccessView(viewHistory.Get(), nullptr, viewHistoryAccess.GetAddressOf()), "The clouds' view history access");
        check(device->CreateShaderResourceView(viewHistory.Get(), nullptr, viewHistoryView.GetAddressOf()), "The clouds' view history view");
        if (FAILED(device->CreatePixelShader(g_cloudShadowPixelShader, sizeof(g_cloudShadowPixelShader), nullptr, shadowPixel.GetAddressOf())))
            throw std::runtime_error("CreatePixelShader failed for the cloud shadow");
        D3D11_TEXTURE2D_DESC shadowDesc{};
        shadowDesc.Width = shadowDesc.Height = ShadowSize;
        shadowDesc.MipLevels = shadowDesc.ArraySize = 1;
        shadowDesc.Format = DXGI_FORMAT_R8_UNORM;
        shadowDesc.SampleDesc.Count = 1;
        shadowDesc.Usage = D3D11_USAGE_DEFAULT;
        shadowDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        shadowDesc.MiscFlags = SharedWithD3D12;
        check(device->CreateTexture2D(&shadowDesc, nullptr, shadowMap.GetAddressOf()), "The cloud shadow map");
        check(device->CreateRenderTargetView(shadowMap.Get(), nullptr, shadowTarget.GetAddressOf()), "The cloud shadow map's target");
        check(device->CreateShaderResourceView(shadowMap.Get(), nullptr, shadowView.GetAddressOf()), "The cloud shadow map's view");
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device->CreateBlendState(&blend, opaqueBlend.GetAddressOf()), "The cloud shadow map's blend state");

        settingsBuffer = makeConstantBuffer(device.Get(), sizeof(ShaderSettings), "Creating the clouds' settings");
        windShader = computeShader(device.Get(), WindSource, "cloud-wind");
        D3D11_BUFFER_DESC windDesc{};
        windDesc.ByteWidth = 32;
        windDesc.Usage = D3D11_USAGE_DEFAULT;
        windDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        windDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        windDesc.StructureByteStride = 16;
        const float zeros[8]{};
        const D3D11_SUBRESOURCE_DATA windData{zeros, 0, 0};
        check(device->CreateBuffer(&windDesc, &windData, windState.GetAddressOf()), "The clouds' wind");
        check(device->CreateUnorderedAccessView(windState.Get(), nullptr, windAccess.GetAddressOf()), "The clouds' wind access");
        check(device->CreateShaderResourceView(windState.Get(), nullptr, windView.GetAddressOf()), "The clouds' wind view");

        // The 3D volume, once: the shape the game's 2D atlas cannot give the clouds.
        const auto levels = cloud_noise::mips(cloud_noise::bake(shippingRecipe()), NoiseSize);
        D3D11_TEXTURE3D_DESC noise{};
        noise.Width = noise.Height = noise.Depth = NoiseSize;
        noise.MipLevels = static_cast<UINT>(levels.size());
        noise.Format = DXGI_FORMAT_R8_UNORM;
        noise.Usage = D3D11_USAGE_IMMUTABLE;
        noise.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> data;
        for (std::size_t level = 0; level < levels.size(); ++level)
        {
            const UINT side = static_cast<UINT>(NoiseSize >> level);
            data.push_back({levels[level].data(), side, side * side});
        }
        check(device->CreateTexture3D(&noise, data.data(), noiseTexture.GetAddressOf()), "The clouds' 3D noise");
        check(device->CreateShaderResourceView(noiseTexture.Get(), nullptr, noiseView.GetAddressOf()), "The clouds' 3D noise view");
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler, noiseSampler.GetAddressOf()), "The clouds' noise sampler");
#if PHANTOM_FX_DEV_TOOLS
        if (!hot) hot = std::make_unique<HotShader>(PHANTOM_FX_CLOUDS_HLSL, "ps_5_0");
        if (!hotShadow) hotShadow = std::make_unique<HotShader>(PHANTOM_FX_CLOUD_SHADOW_HLSL, "ps_5_0");
#endif
    }
    catch (const std::exception& error)
    {
        hasFailed = true;
        log(std::string("Clouds: ") + error.what());
    }
}

// Whether the draw is the main view's: the viewport covers the render target, and the scene's depth the game binds is
// as large. A smaller camera (a mirror) keeps the dome draw, which needs no depth.
bool isMainView(ID3D11DeviceContext* context)
{
    UINT count = 1;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&count, &viewport);
    ComPtr<ID3D11RenderTargetView> target;
    context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
    ComPtr<ID3D11ShaderResourceView> depth;
    context->PSGetShaderResources(SceneDepthSlot, 1, depth.GetAddressOf());
    if (!count || !target || !depth) return false;

    ComPtr<ID3D11Resource> targetResource, depthResource;
    target->GetResource(targetResource.GetAddressOf());
    depth->GetResource(depthResource.GetAddressOf());
    ComPtr<ID3D11Texture2D> targetTexture, depthTexture;
    if (FAILED(targetResource.As(&targetTexture)) || FAILED(depthResource.As(&depthTexture))) return false;

    D3D11_TEXTURE2D_DESC targetDesc{}, depthDesc{};
    targetTexture->GetDesc(&targetDesc);
    depthTexture->GetDesc(&depthDesc);

    return targetDesc.SampleDesc.Count == 1 && depthDesc.SampleDesc.Count == 1 && viewport.TopLeftX == 0 && viewport.TopLeftY == 0
        && static_cast<UINT>(viewport.Width) == targetDesc.Width && static_cast<UINT>(viewport.Height) == targetDesc.Height
        && depthDesc.Width == targetDesc.Width && depthDesc.Height == targetDesc.Height;
}

// One triangle over the target with the clouds' pixel shader, the game's other states kept: its blend, its render target.
void drawOverScene(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11VertexShader> gameVertex;
    ComPtr<ID3D11InputLayout> gameLayout;
    D3D11_PRIMITIVE_TOPOLOGY gameTopology{};
    ComPtr<ID3D11DepthStencilState> gameDepth;
    UINT stencilReference{};
    ComPtr<ID3D11RasterizerState> gameRaster;
    context->VSGetShader(gameVertex.GetAddressOf(), nullptr, nullptr);
    context->IAGetInputLayout(gameLayout.GetAddressOf());
    context->IAGetPrimitiveTopology(&gameTopology);
    context->OMGetDepthStencilState(gameDepth.GetAddressOf(), &stencilReference);
    context->RSGetState(gameRaster.GetAddressOf());

    context->VSSetShader(screenVertex.Get(), nullptr, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->OMSetDepthStencilState(noDepth.Get(), 0);
    context->RSSetState(noCulling.Get());
    context->Draw(3, 0);

    context->VSSetShader(gameVertex.Get(), nullptr, 0);
    context->IASetInputLayout(gameLayout.Get());
    context->IASetPrimitiveTopology(gameTopology);
    context->OMSetDepthStencilState(gameDepth.Get(), stencilReference);
    context->RSSetState(gameRaster.Get());
}

// The half-size targets, made again when the render size changes. Call with resourceMutex held.
bool ensureLowTargets(UINT width, UINT height)
{
    const UINT wantedWidth = (width + LowScale - 1) / LowScale, wantedHeight = (height + LowScale - 1) / LowScale;
    if (lowColor && wantedWidth == lowWidth && wantedHeight == lowHeight) return true;

    lowColor.Reset();
    lowDistance.Reset();
    lowColorTarget.Reset();
    lowDistanceTarget.Reset();
    lowColorView.Reset();
    lowDistanceView.Reset();
    for (UINT i = 0; i < 2; ++i)
    {
        history[i].Reset();
        historyTarget[i].Reset();
        historyView[i].Reset();
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = wantedWidth;
    desc.Height = wantedHeight;
    desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, lowColor.GetAddressOf()))) return false;
    for (UINT i = 0; i < 2; ++i)
        if (FAILED(device->CreateTexture2D(&desc, nullptr, history[i].GetAddressOf()))
            || FAILED(device->CreateRenderTargetView(history[i].Get(), nullptr, historyTarget[i].GetAddressOf()))
            || FAILED(device->CreateShaderResourceView(history[i].Get(), nullptr, historyView[i].GetAddressOf())))
        {
            lowColor.Reset();
            return false;
        }
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, lowDistance.GetAddressOf()))) return false;
    if (FAILED(device->CreateRenderTargetView(lowColor.Get(), nullptr, lowColorTarget.GetAddressOf()))
        || FAILED(device->CreateRenderTargetView(lowDistance.Get(), nullptr, lowDistanceTarget.GetAddressOf()))
        || FAILED(device->CreateShaderResourceView(lowColor.Get(), nullptr, lowColorView.GetAddressOf()))
        || FAILED(device->CreateShaderResourceView(lowDistance.Get(), nullptr, lowDistanceView.GetAddressOf())))
    {
        lowColor.Reset();
        return false;
    }

    lowWidth = wantedWidth;
    lowHeight = wantedHeight;
    log("Clouds: marched at " + std::to_string(lowWidth) + " x " + std::to_string(lowHeight) + ".");
    return true;
}

// The clouds' march into the half-size targets, then the composite over the game's target with its blend. False when the
// targets could not be made: the march's pixels are half-size blocks, so it cannot simply run at full size.
bool drawAtHalfSize(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11RenderTargetView> gameTarget;
    ComPtr<ID3D11DepthStencilView> gameDepthView;
    context->OMGetRenderTargets(1, gameTarget.GetAddressOf(), gameDepthView.GetAddressOf());
    if (!gameTarget || !compositePixel || !resolvePixel || !viewHistoryView) return false;
    ComPtr<ID3D11Resource> targetResource;
    gameTarget->GetResource(targetResource.GetAddressOf());
    ComPtr<ID3D11Texture2D> targetTexture;
    if (FAILED(targetResource.As(&targetTexture))) return false;
    D3D11_TEXTURE2D_DESC targetDesc{};
    targetTexture->GetDesc(&targetDesc);
    {
        std::lock_guard lock(resourceMutex);
        if (!ensureLowTargets(targetDesc.Width, targetDesc.Height)) return false;
    }

    UINT viewports = 1;
    D3D11_VIEWPORT gameViewport{};
    context->RSGetViewports(&viewports, &gameViewport);
    ComPtr<ID3D11BlendState> gameBlend;
    FLOAT blendFactor[4]{};
    UINT sampleMask{};
    context->OMGetBlendState(gameBlend.GetAddressOf(), blendFactor, &sampleMask);

    ID3D11RenderTargetView* const lowTargets[]{lowColorTarget.Get(), lowDistanceTarget.Get()};
    context->OMSetRenderTargets(2, lowTargets, nullptr);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(lowWidth), static_cast<float>(lowHeight), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->OMSetBlendState(opaqueBlend.Get(), nullptr, 0xffffffff);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    drawOverScene(context);

    // The history: this frame's march blended into the last, at the same size.
    ID3D11ShaderResourceView* gameViews[4]{};
    context->PSGetShaderResources(LowColorSlot, 4, gameViews);
    const UINT next = historyIndex ^ 1;
    auto* resolveTarget = historyTarget[next].Get();
    context->OMSetRenderTargets(1, &resolveTarget, nullptr);
    ID3D11ShaderResourceView* const resolveViews[]{lowColorView.Get(), nullptr, historyView[historyIndex].Get(), viewHistoryView.Get()};
    context->PSSetShaderResources(LowColorSlot, 4, resolveViews);
    context->PSSetShader(resolvePixel.Get(), nullptr, 0);
    drawOverScene(context);

    ID3D11RenderTargetView* restoreTarget = gameTarget.Get();
    context->OMSetRenderTargets(1, &restoreTarget, gameDepthView.Get());
    context->RSSetViewports(1, &gameViewport);
    context->OMSetBlendState(gameBlend.Get(), blendFactor, sampleMask);
    ID3D11ShaderResourceView* const lowViews[]{historyView[next].Get(), lowDistanceView.Get(), nullptr, nullptr};
    context->PSSetShaderResources(LowColorSlot, 4, lowViews);
    context->PSSetShader(compositePixel.Get(), nullptr, 0);
    drawOverScene(context);
    context->PSSetShaderResources(LowColorSlot, 4, gameViews);
    historyIndex = next;

    // This frame's view, for the next frame's history.
    if (viewCopyShader && viewHistoryAccess)
    {
        ComPtr<ID3D11Buffer> scene;
        context->PSGetConstantBuffers(2, 1, scene.GetAddressOf());
        SavedComputeState saved(context, {.buffers = 3, .accesses = 1});
        ID3D11Buffer* const buffers[3]{nullptr, nullptr, scene.Get()};
        context->CSSetConstantBuffers(0, 3, buffers);
        auto* access = viewHistoryAccess.Get();
        context->CSSetUnorderedAccessViews(0, 1, &access, nullptr);
        context->CSSetShader(viewCopyShader.Get(), nullptr, 0);
        context->Dispatch(1, 1, 1);
    }
    for (auto* view : gameViews)
        if (view) view->Release();
    context->PSSetShader(pixel.Get(), nullptr, 0);
    return true;
}


// The shadow map, drawn with the game's own cloud constants (still bound for its cloud draw): one triangle over a square
// target. The game's render target, viewport, blend and the rest are put back after.
void renderShadowMap(ID3D11DeviceContext* context)
{
    if (!shadowPixel || !shadowTarget || !screenVertex || !opaqueBlend) return;

    ComPtr<ID3D11RenderTargetView> gameTarget;
    ComPtr<ID3D11DepthStencilView> gameDepthView;
    context->OMGetRenderTargets(1, gameTarget.GetAddressOf(), gameDepthView.GetAddressOf());
    UINT viewports = 1;
    D3D11_VIEWPORT gameViewport{};
    context->RSGetViewports(&viewports, &gameViewport);
    ComPtr<ID3D11BlendState> gameBlend;
    FLOAT blendFactor[4]{};
    UINT sampleMask{};
    context->OMGetBlendState(gameBlend.GetAddressOf(), blendFactor, &sampleMask);
    ComPtr<ID3D11PixelShader> gamePixel;
    context->PSGetShader(gamePixel.GetAddressOf(), nullptr, nullptr);

    // The map is not read while it is drawn.
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(CloudShadowSlot, 1, &none);
    auto* target = shadowTarget.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(ShadowSize), static_cast<float>(ShadowSize), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->OMSetBlendState(opaqueBlend.Get(), nullptr, 0xffffffff);
    context->PSSetShader(shadowPixel.Get(), nullptr, 0);
    drawOverScene(context);

    ID3D11RenderTargetView* restoreTarget = gameTarget.Get();
    context->OMSetRenderTargets(1, &restoreTarget, gameDepthView.Get());
    context->RSSetViewports(1, &gameViewport);
    context->OMSetBlendState(gameBlend.Get(), blendFactor, sampleMask);
    context->PSSetShader(gamePixel.Get(), nullptr, 0);
    auto* view = shadowView.Get();
    context->PSSetShaderResources(CloudShadowSlot, 1, &view);
    shadowDrawn.store(true, std::memory_order_relaxed);
}
}

Health health()
{
    if (!isOn.load(std::memory_order_relaxed)) return {};
    if (hasFailed) return Health::unavailable("The clouds are unavailable (see PhantomFX.log).");
    if (!drawn.load(std::memory_order_relaxed)) return Health::waiting("The clouds show where the game draws its sky.");

    return {};
}

ID3D11Texture2D* shadowTexture() noexcept
{
    return isOn.load(std::memory_order_relaxed) && shadowDrawn.load(std::memory_order_relaxed) ? shadowMap.Get() : nullptr;
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    ensurePipeline();
}

void configure(const Configuration& configuration) noexcept
{
    const auto& settings = configuration.settings.clouds;
    const bool on = configuration.effects.runs(Effect::Clouds);
    isOn.store(on, std::memory_order_relaxed);
    const float qualitySteps = settings.quality == CloudQuality::Low ? 64.0f
        : settings.quality == CloudQuality::Medium ? 128.0f
        : settings.quality == CloudQuality::Ultra ? 256.0f
        : 192.0f;
    steps.store(qualitySteps, std::memory_order_relaxed);
    coverage.store(settings.coverage, std::memory_order_relaxed);
    size.store(settings.size, std::memory_order_relaxed);
    wind.store(settings.wind, std::memory_order_relaxed);
    shadow.store(settings.shadow, std::memory_order_relaxed);
    fog.store(settings.fog ? 1.0f : 0.0f, std::memory_order_relaxed);
    if (!on) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

void detach() noexcept
{
    std::lock_guard lock(resourceMutex);
    pixel.Reset();
    settingsBuffer.Reset();
    windShader.Reset();
    windView.Reset();
    windAccess.Reset();
    windState.Reset();
    shadowPixel.Reset();
    shadowView.Reset();
    shadowTarget.Reset();
    shadowMap.Reset();
    shadowDrawn.store(false, std::memory_order_relaxed);
    opaqueBlend.Reset();
    noiseView.Reset();
    noiseTexture.Reset();
    noiseSampler.Reset();
    compositePixel.Reset();
    resolvePixel.Reset();
    viewCopyShader.Reset();
    viewHistory.Reset();
    viewHistoryAccess.Reset();
    viewHistoryView.Reset();
    for (UINT i = 0; i < 2; ++i)
    {
        history[i].Reset();
        historyTarget[i].Reset();
        historyView[i].Reset();
    }
    lowColor.Reset();
    lowDistance.Reset();
    lowColorTarget.Reset();
    lowDistanceTarget.Reset();
    lowColorView.Reset();
    lowDistanceView.Reset();
    lowWidth = lowHeight = 0;
    screenVertex.Reset();
    noDepth.Reset();
    noCulling.Reset();
#if PHANTOM_FX_DEV_TOOLS
    hot.reset();
    hotShadow.reset();
#endif
    device.Reset();
    isOn.store(false, std::memory_order_relaxed);
    drawn.store(false, std::memory_order_relaxed);
    hasFailed = false;
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!call.indexed) return false;

    const auto* row = shader.row();
    if (!row || row->pass != game_shaders::GamePass::SkyDome) return false;

    // The game's painted dome (tpp3dfw_constant_sky) carries its own wisps of cloud, which would show through the deck as a second,
    // soft layer; the deck is the sky's clouds, so the dome is left out.
    if (DomeChecksum == row->checksum) return isOn.load(std::memory_order_relaxed);
    if (CloudChecksum != row->checksum) return false;

    if (!isOn.load(std::memory_order_relaxed))
    {
        static bool noted{};
        if (!noted) log("Clouds: the game's cloud draw is left alone; the switch is off.");
        noted = true;
        return false;
    }

    static bool replaced{};
    if (!replaced) log("Clouds: the game's cloud draw is replaced.");
    replaced = true;

    ComPtr<ID3D11PixelShader> gamePixel;
    context->PSGetShader(gamePixel.GetAddressOf(), nullptr, nullptr);
    {
        std::lock_guard lock(resourceMutex);
#if PHANTOM_FX_DEV_TOOLS
        if (hot)
            if (auto fresh = hot->takePixelShader(device.Get())) pixel = std::move(fresh);
        if (hotShadow)
            if (auto fresh = hotShadow->takePixelShader(device.Get())) shadowPixel = std::move(fresh);
#endif
        if (!pixel) return false;
        context->PSSetShader(pixel.Get(), nullptr, 0);
    }

    // The menu's quality and tweakables, for the draw and the shadow map it draws first.
    ComPtr<ID3D11Buffer> gameSettings;
    context->PSGetConstantBuffers(SettingsSlot, 1, gameSettings.GetAddressOf());
    {
        static std::chrono::steady_clock::time_point lastDraw{};
        static std::uint32_t drawCount = 0;
        static float weatherCover = -1;
        const auto now = std::chrono::steady_clock::now();
        const float seconds = lastDraw.time_since_epoch().count() && !weather::isGamePaused() ? std::min(std::chrono::duration<float>(now - lastDraw).count(), 0.1f) : 0;
        lastDraw = now;
        const float coverTarget = CoverOf(weather::state().requested.value_or(weather::Type::Sunny));
        weatherCover = weatherCover < 0 ? coverTarget : weatherCover + (coverTarget - weatherCover) * std::min(seconds / 30, 1.0f);
        // The game's clock sets the pace: normal time drifts them at the wind's own speed, faster time (a sleep, the cigar) faster, a
        // stopped clock not at all. A jump of the clock (set from the menu, or a mission's) is capped so they do not leap.
        static float lastClock = -1;
        const auto sky = weather::state();
        float driftSeconds = seconds;
        if (sky.available)
        {
            float passed = lastClock < 0 ? 0 : sky.clock - lastClock;
            if (passed < -SecondsPerDay / 2) passed += SecondsPerDay; // Past midnight
            driftSeconds = seconds > 0 ? std::clamp(passed / NormalClockRate, 0.0f, seconds * MostClockSpeedUp) : 0;
            lastClock = sky.clock;
        }
        else lastClock = -1;
        const float travel = driftSeconds * CloudWindScale * wind.load(std::memory_order_relaxed)
            * WindOf(weather::state().requested.value_or(weather::Type::Sunny));
        const ShaderSettings values{steps.load(std::memory_order_relaxed), coverage.load(std::memory_order_relaxed),
            size.load(std::memory_order_relaxed), wind.load(std::memory_order_relaxed), shadow.load(std::memory_order_relaxed),
            travel * WindDirectionX, travel * WindDirectionZ, fog.load(std::memory_order_relaxed), static_cast<float>(drawCount++ % 1024), weatherCover, {}};
        std::lock_guard lock(resourceMutex);
        if (settingsBuffer && !FAILED(write(context, settingsBuffer.Get(), values)))
        {
            auto* buffer = settingsBuffer.Get();
            context->PSSetConstantBuffers(SettingsSlot, 1, &buffer);

            // The drift: the change in the game's cloud offset since the last draw, at the slider's rate.
            if (windShader && windAccess)
            {
                ComPtr<ID3D11Buffer> material;
                context->PSGetConstantBuffers(4, 1, material.GetAddressOf());
                SavedComputeState saved(context, {.buffers = 8, .accesses = 1});
                ID3D11Buffer* const buffers[8]{nullptr, nullptr, nullptr, nullptr, material.Get(), nullptr, nullptr, buffer};
                context->CSSetConstantBuffers(0, 8, buffers);
                auto* access = windAccess.Get();
                context->CSSetUnorderedAccessViews(0, 1, &access, nullptr);
                context->CSSetShader(windShader.Get(), nullptr, 0);
                context->Dispatch(1, 1, 1);
            }
        }
    }
    ID3D11ShaderResourceView* gameWind = nullptr;
    context->PSGetShaderResources(WindSlot, 1, &gameWind);
    auto* windRead = windView.Get();
    context->PSSetShaderResources(WindSlot, 1, &windRead);

    // The 3D noise replaces whatever the game had at the slot for the draw and the sky capture.
    ID3D11ShaderResourceView* gameNoise = nullptr;
    context->PSGetShaderResources(CloudNoiseSlot, 1, &gameNoise);
    auto* noise = noiseView.Get();
    context->PSSetShaderResources(CloudNoiseSlot, 1, &noise);
    ID3D11SamplerState* gameSampler = nullptr;
    context->PSGetSamplers(NoiseSamplerSlot, 1, &gameSampler);
    auto* sampler = noiseSampler.Get();
    context->PSSetSamplers(NoiseSamplerSlot, 1, &sampler);
    ID3D11ShaderResourceView* gameSky = nullptr;
    context->PSGetShaderResources(SkyParametersSlot, 1, &gameSky);
    auto* sky = atmosphere::skyParametersView();
    context->PSSetShaderResources(SkyParametersSlot, 1, &sky);

    // The main view: over the whole scene, reading its depth, so the clouds meet the terrain, the aircraft and the eye.
    // Anything else (a mirror) keeps the dome, drawn as the game does, without the depth.
    if (screenVertex && isMainView(context))
    {
        performance::Timed timed(context, performance::Pass::CloudShadow);
        renderShadowMap(context);
        timed.next(performance::Pass::Clouds);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        if (!drawAtHalfSize(context))
        {
            static bool noted{};
            if (!noted) log("Clouds: the half-size targets could not be made; the clouds are left out.");
            noted = true;
        }
        drawn.store(true, std::memory_order_relaxed);
    }
    else context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    // The rays see the sky the player sees: capture the dome draw too, with the add-on's pixel shader still bound.
    sky_capture::capture(context, call);
    ID3D11ShaderResourceView* noShadow = nullptr;
    context->PSSetShaderResources(CloudShadowSlot, 1, &noShadow);
    context->PSSetShaderResources(CloudNoiseSlot, 1, &gameNoise);
    if (gameNoise) gameNoise->Release();
    context->PSSetSamplers(NoiseSamplerSlot, 1, &gameSampler);
    if (gameSampler) gameSampler->Release();
    context->PSSetShaderResources(SkyParametersSlot, 1, &gameSky);
    if (gameSky) gameSky->Release();
    context->PSSetShaderResources(WindSlot, 1, &gameWind);
    if (gameWind) gameWind->Release();
    context->PSSetShader(gamePixel.Get(), nullptr, 0);
    context->PSSetConstantBuffers(SettingsSlot, 1, gameSettings.GetAddressOf());

    return true;
}
}
