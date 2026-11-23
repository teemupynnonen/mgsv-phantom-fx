// Better clouds: clouds.hpp describes them.
#include "features/clouds/clouds.hpp"

#include "d3d11_helpers.hpp"
#include "features/path_tracing/sky_capture.hpp"
#include "game_shaders.hpp"
#include "log.hpp"
#include "phantom_fx_settings.hpp"
#include "phantom_shaders/cloud_shadow.hpp"
#include "phantom_shaders/clouds.hpp"
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

std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> pixel;
// The full-screen draw: its vertex shader, and states without a depth test and without culling.
ComPtr<ID3D11VertexShader> screenVertex;
ComPtr<ID3D11DepthStencilState> noDepth;
ComPtr<ID3D11RasterizerState> noCulling;
// The slot the game binds the scene's depth at for its cloud draw, and the one the add-on's 3D noise takes.
constexpr UINT SceneDepthSlot = 13, CloudNoiseSlot = 14;
// A 64-cube of value-noise fbm (generateNoise): the game's own cloud texture has eight slices.
constexpr int NoiseSize = 64;
ComPtr<ID3D11Texture3D> noiseTexture;
ComPtr<ID3D11ShaderResourceView> noiseView;
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
    // The clouds' own travelled distance in metres (the wind setting's, integrated here): a slider that changes the speed
    // must not move the clouds, only their pace.
    float offsetX, offsetZ;
    float fog; // 1 the game's fog over the clouds, 0 none
};
static_assert(sizeof(ShaderSettings) % 16 == 0);
std::atomic<float> steps{192}, coverage{1}, size{1}, wind{1}, shadow{1}, fog{1};
// The clouds' drift (cloud_wind below): the game's own cloud offset, which moves only with game time and its weather's wind,
// taken a change at a time and scaled by the Wind setting, so a paused game holds them and the slider changes their pace without
// moving them. A structured buffer of two float4s: [0].xy the distance travelled in metres, [0].zw the game's last offset in
// kilometres; [1].x 1 once it holds one.
constexpr UINT WindSlot = 17;
ComPtr<ID3D11ComputeShader> windShader;
ComPtr<ID3D11Buffer> windState;
ComPtr<ID3D11UnorderedAccessView> windAccess;
ComPtr<ID3D11ShaderResourceView> windView;
constexpr const char* WindSource = R"(
cbuffer cPSMaterial : register(b4) { float4 m_materials[8]; };
cbuffer cCloudSettings : register(b7) { float g_cloudSteps, g_cloudCoverage, g_cloudSize, g_cloudWind, g_cloudShadow, g_padding[3]; };
RWStructuredBuffer<float4> State : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    float4 travelled = State[0];
    float2 game = m_materials[5].zw;
    float2 change = game - travelled.zw;
    // The first frame, or a jump (a load, a new weather): no drift from it.
    if (State[1].x == 0 || any(abs(change) > 0.5)) change = 0;
    travelled.xy += change * 1000 * g_cloudWind;
    travelled.zw = game;
    State[0] = travelled;
    State[1] = float4(1, 0, 0, 0);
}
)";

std::uint32_t hash3(int x, int y, int z)
{
    std::uint32_t v = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u
        + static_cast<std::uint32_t>(z) * 2147483647u;
    v = (v ^ (v >> 13)) * 1274126177u;
    return v ^ (v >> 16);
}

float lattice(int x, int y, int z, int period)
{
    const auto wrap = [period](int v) { return ((v % period) + period) % period; };
    return static_cast<float>(hash3(wrap(x), wrap(y), wrap(z)) & 0xff) / 255.0f;
}

float valueNoise(float x, float y, float z, int period)
{
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y)), zi = static_cast<int>(std::floor(z));
    const float fx = x - xi, fy = y - yi, fz = z - zi;
    const auto smooth = [](float t) { return t * t * (3 - 2 * t); };
    const float u = smooth(fx), v = smooth(fy), w = smooth(fz);
    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    const float c000 = lattice(xi, yi, zi, period), c100 = lattice(xi + 1, yi, zi, period);
    const float c010 = lattice(xi, yi + 1, zi, period), c110 = lattice(xi + 1, yi + 1, zi, period);
    const float c001 = lattice(xi, yi, zi + 1, period), c101 = lattice(xi + 1, yi, zi + 1, period);
    const float c011 = lattice(xi, yi + 1, zi + 1, period), c111 = lattice(xi + 1, yi + 1, zi + 1, period);
    return mix(mix(mix(c000, c100, u), mix(c010, c110, u), v), mix(mix(c001, c101, u), mix(c011, c111, u), v), w);
}

std::vector<std::uint8_t> generateNoise()
{
    std::vector<std::uint8_t> voxels(static_cast<std::size_t>(NoiseSize) * NoiseSize * NoiseSize);
    for (int z = 0; z < NoiseSize; ++z)
        for (int y = 0; y < NoiseSize; ++y)
            for (int x = 0; x < NoiseSize; ++x)
            {
                const float u = static_cast<float>(x) / NoiseSize, v = static_cast<float>(y) / NoiseSize, w = static_cast<float>(z) / NoiseSize;
                float n = 0, amplitude = 0.5f, total = 0;
                for (const int period : {4, 8, 16, 32})
                {
                    n += valueNoise(u * period, v * period, w * period, period) * amplitude;
                    total += amplitude;
                    amplitude *= 0.5f;
                }
                n /= total;
                // Billows: the first octave's rounded side turned into the cloud's cauliflower.
                n = std::clamp(0.75f * n + 0.25f * (1 - std::fabs(2 * n - 1)), 0.0f, 1.0f);
                // FBM averages toward its middle, and this one's median is 0.63 with a narrow spread: centre the
                // contrast on that median and gain it up, so a threshold has real clear air to pick.
                n = std::clamp((n - 0.63f) * 6.0f + 0.5f, 0.0f, 1.0f);
                voxels[(static_cast<std::size_t>(z) * NoiseSize + y) * NoiseSize + x] = static_cast<std::uint8_t>(n * 255);
            }

    return voxels;
}
#if PHANTOM_FX_DEV_TOOLS
std::unique_ptr<HotShader> hot; // Watches clouds.hlsl; a saved edit replaces `pixel` at the next cloud draw
std::unique_ptr<HotShader> hotShadow; // The same for cloud_shadow.hlsl and `shadowPixel`
#endif
std::atomic<bool> isOn{};
bool hasFailed{};

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
        const auto voxels = generateNoise();
        D3D11_TEXTURE3D_DESC noise{};
        noise.Width = noise.Height = noise.Depth = NoiseSize;
        noise.MipLevels = 1;
        noise.Format = DXGI_FORMAT_R8_UNORM;
        noise.Usage = D3D11_USAGE_IMMUTABLE;
        noise.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{voxels.data(), NoiseSize, NoiseSize * NoiseSize};
        check(device->CreateTexture3D(&noise, &data, noiseTexture.GetAddressOf()), "The clouds' 3D noise");
        check(device->CreateShaderResourceView(noiseTexture.Get(), nullptr, noiseView.GetAddressOf()), "The clouds' 3D noise view");
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
    const bool on = configuration.areEnhancementsOn && settings.on;
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
    screenVertex.Reset();
    noDepth.Reset();
    noCulling.Reset();
#if PHANTOM_FX_DEV_TOOLS
    hot.reset();
    hotShadow.reset();
#endif
    device.Reset();
    isOn.store(false, std::memory_order_relaxed);
    hasFailed = false;
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!call.indexed) return false;

    const auto* row = shader.row();
    if (!row || row->pass != game_shaders::GamePass::SkyDome || CloudChecksum != row->checksum) return false;

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
        const ShaderSettings values{steps.load(std::memory_order_relaxed), coverage.load(std::memory_order_relaxed),
            size.load(std::memory_order_relaxed), wind.load(std::memory_order_relaxed), shadow.load(std::memory_order_relaxed),
            0, 0, fog.load(std::memory_order_relaxed)};
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

    // The main view: over the whole scene, reading its depth, so the clouds meet the terrain, the aircraft and the eye.
    // Anything else (a mirror) keeps the dome, drawn as the game does, without the depth.
    if (screenVertex && isMainView(context))
    {
        renderShadowMap(context);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        drawOverScene(context);
    }
    else context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    // The rays see the sky the player sees: capture the dome draw too, with the add-on's pixel shader still bound.
    sky_capture::capture(context, call);
    ID3D11ShaderResourceView* noShadow = nullptr;
    context->PSSetShaderResources(CloudShadowSlot, 1, &noShadow);
    context->PSSetShaderResources(CloudNoiseSlot, 1, &gameNoise);
    if (gameNoise) gameNoise->Release();
    context->PSSetShaderResources(WindSlot, 1, &gameWind);
    if (gameWind) gameWind->Release();
    context->PSSetShader(gamePixel.Get(), nullptr, 0);
    context->PSSetConstantBuffers(SettingsSlot, 1, gameSettings.GetAddressOf());

    return true;
}
}
