#include "features/water_caustics/water_caustics.hpp"

#include "clock.hpp"
#include "d3d11_helpers.hpp"
#include "features/water_caustics/caustics_map.hpp"
#include "features/water_caustics/water_caustics_shaders.hpp"
#include "frame_latch.hpp"
#include "hresult.hpp"
#include "log.hpp"
#include "passes/forward_scene.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "performance.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace phantom_fx::water_caustics
{
namespace
{
using Microsoft::WRL::ComPtr;

struct Latched
{
    WaterCaustics settings;
    bool isRunning{};
};

FrameLatch<Latched> latch;

// Slots none of the river shaders binds, so everything of the game's stays where shaders::Caustics reads it.
constexpr UINT ParametersSlot = 6, MapSlot = 7, SunShadowSlot = 8;

struct Parameters
{
    float intensity, hasSunShadow, padding[2];
    float scroll[4];
};

// How fast the web's two layers drift, in tiles a second: against each other, so their crossings keep changing. Real time, as
// the sea's waves keep: on a river's own scroll, which stops with the game, the webs stood still through a pause.
constexpr double Drift[4]{0.015, 0.007, -0.011, -0.014};

LONGLONG lastPresent{};
std::atomic<double> driftSeconds{};

// ---- Direct3D, guarded by resourceMutex, never held through a Direct3D call on a recording thread ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> shader;
ComPtr<ID3D11BlendState> lightenScene;
ComPtr<ID3D11DepthStencilState> depthTest;
ComPtr<ID3D11Buffer> parameters;
ComPtr<ID3D11ShaderResourceView> map;
std::atomic<bool> hasPipelineFailed{};

// The map with its mips, as one immutable texture: mips so deep water can read it softer, and a river seen far does not shimmer.
ComPtr<ID3D11ShaderResourceView> makeMapView()
{
    std::vector<std::vector<std::uint8_t>> mips{makeCausticsMap()};
    for (auto size = CausticsMapSize / 2; size >= 1; size /= 2)
    {
        const auto& above = mips.back();
        std::vector<std::uint8_t> mip(static_cast<std::size_t>(size) * size);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const auto texel = [&](int column, int row) { return static_cast<int>(above[static_cast<std::size_t>(row) * size * 2 + column]); };
                mip[static_cast<std::size_t>(y) * size + x] = static_cast<std::uint8_t>(
                    (texel(x * 2, y * 2) + texel(x * 2 + 1, y * 2) + texel(x * 2, y * 2 + 1) + texel(x * 2 + 1, y * 2 + 1) + 2) / 4);
            }
        mips.push_back(std::move(mip));
    }

    std::vector<D3D11_SUBRESOURCE_DATA> data;
    auto pitch = static_cast<UINT>(CausticsMapSize);
    for (const auto& mip : mips)
    {
        data.push_back({mip.data(), pitch, 0});
        pitch /= 2;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = CausticsMapSize;
    desc.MipLevels = static_cast<UINT>(mips.size());
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    check(device->CreateTexture2D(&desc, data.data(), texture.GetAddressOf()), "Creating the caustics map");
    check(device->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf()), "Creating the caustics map's view");

    return view;
}

// Compiles the shader and makes the states once; call with resourceMutex held.
void ensurePipeline()
{
    if (shader || !device || hasPipelineFailed) return;

    try
    {
        parameters = makeConstantBuffer(device.Get(), sizeof(Parameters), "Creating the caustics' constants");
        map = makeMapView();

        D3D11_BLEND_DESC blend{};
        auto& target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        // scene x (1 + the shader's), which brightens on the game's 8-bit scene too: there a colour past 1 would not reach the blend.
        target.SrcBlend = D3D11_BLEND_DEST_COLOR;
        target.DestBlend = D3D11_BLEND_ONE;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        check(device->CreateBlendState(&blend, lightenScene.ReleaseAndGetAddressOf()), "Creating the caustics' blend");

        // As the river is tested, reversed depth, without writing.
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        check(device->CreateDepthStencilState(&depth, depthTest.ReleaseAndGetAddressOf()), "Creating the caustics' depth test");

        // Last, so it marks a complete pipeline.
        shader = pixelShader(device.Get(), (std::string(forward_scene::Common) + shaders::Caustics).c_str(), "water-caustics");
    }
    catch (const std::exception& error)
    {
        hasPipelineFailed = true;
        log(std::string("The water's caustics are unavailable: ") + error.what());
    }
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (latch.latest().isRunning) ensurePipeline();
}

void configure(const Configuration& configuration) noexcept
{
    latch.set({configuration.settings.waterCaustics, configuration.effects.runs(Effect::WaterCaustics)});
    if (!latch.latest().isRunning) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

void present() noexcept
{
    latch.present();

    const auto ticks = clock::ticksNow();
    const double elapsed = lastPresent ? std::clamp(clock::millisecondsOf(ticks - lastPresent) / 1000, 0.0, 0.1) : 0;
    lastPresent = ticks;
    driftSeconds.store(driftSeconds.load(std::memory_order_relaxed) + elapsed, std::memory_order_relaxed);
}

void detach() noexcept
{
    std::lock_guard lock(resourceMutex);
    shader.Reset();
    lightenScene.Reset();
    depthTest.Reset();
    parameters.Reset();
    map.Reset();
    device.Reset();
    hasPipelineFailed = false;
}

Health health()
{
    if (latch.latest().isRunning && hasPipelineFailed) return Health::unavailable("The water's caustics are unavailable (see PhantomFX.log).");

    return {};
}

void draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    try
    {
        const auto [settings, isRunning] = latch.frame();
        if (!isRunning) return;

        ComPtr<ID3D11PixelShader> causticShader;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11Buffer> constants;
        ComPtr<ID3D11ShaderResourceView> web;
        {
            std::lock_guard lock(resourceMutex);
            if (!shader) return;

            causticShader = shader;
            blend = lightenScene;
            depth = depthTest;
            constants = parameters;
            web = map;
        }

        ComPtr<ID3D11RenderTargetView> target;
        context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        const auto [width, height] = sizeOf(target.Get());
        auto* sunShadow = ray_tracing::sunShadow(width, height);

        Parameters values{.intensity = settings.intensity, .hasSunShadow = sunShadow ? 1.0f : 0.0f};
        // Whole tiles dropped, so hours of play keep a float's precision.
        const auto seconds = driftSeconds.load(std::memory_order_relaxed);
        for (int layer = 0; layer < 4; ++layer) values.scroll[layer] = static_cast<float>(std::fmod(seconds * Drift[layer], 1.0));
        check(write(context, constants.Get(), values), "Writing the caustics' constants");

        const performance::Timed timed(context, performance::Pass::WaterCaustics);

        ComPtr<ID3D11PixelShader> gameShader;
        ComPtr<ID3D11DepthStencilState> gameDepth;
        ComPtr<ID3D11Buffer> gameConstants;
        ComPtr<ID3D11ShaderResourceView> gameMap, gameSunShadow;
        UINT stencilReference{};
        const auto gameBlend = boundBlend(context);
        context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
        context->OMGetDepthStencilState(gameDepth.GetAddressOf(), &stencilReference);
        context->PSGetConstantBuffers(ParametersSlot, 1, gameConstants.GetAddressOf());
        context->PSGetShaderResources(MapSlot, 1, gameMap.GetAddressOf());
        context->PSGetShaderResources(SunShadowSlot, 1, gameSunShadow.GetAddressOf());

        context->PSSetShader(causticShader.Get(), nullptr, 0);
        context->PSSetConstantBuffers(ParametersSlot, 1, constants.GetAddressOf());
        context->PSSetShaderResources(MapSlot, 1, web.GetAddressOf());
        context->PSSetShaderResources(SunShadowSlot, 1, &sunShadow);
        context->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(depth.Get(), 0);
        context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);

        context->OMSetDepthStencilState(gameDepth.Get(), stencilReference);
        context->OMSetBlendState(gameBlend.state.Get(), gameBlend.factor, gameBlend.sampleMask);
        context->PSSetShaderResources(SunShadowSlot, 1, gameSunShadow.GetAddressOf());
        context->PSSetShaderResources(MapSlot, 1, gameMap.GetAddressOf());
        context->PSSetConstantBuffers(ParametersSlot, 1, gameConstants.GetAddressOf());
        context->PSSetShader(gameShader.Get(), nullptr, 0);
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("The water's caustics: nothing this frame: ") + error.what());
    }
}
}
