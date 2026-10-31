// Path tracing on Direct3D 11; path_tracing.hpp describes it.
#include "features/path_tracing/path_tracing.hpp"

#include "d3d11_helpers.hpp"
#include "features/path_tracing/glass.hpp"
#include "features/path_tracing/path_tracing_shaders.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"
#include "passes/sun_light_pass.hpp"
#include "performance.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace phantom_fx::path_tracing
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_shaders::GamePass;

constexpr UINT MaskDepthSlot = 0; // The reflections' mask's inDepthImage
// The blend's additions (shaders::ReflectionBlend): the depths, the reflection map, the reflections and glass's weights; its
// constants.
constexpr UINT BlendViewSlot = 3, BlendViews = 5, BlendParameterSlot = 1;
constexpr ComputeSlots PassSlots{.buffers = 1, .views = 2, .accesses = 1};

struct Parameters
{
    std::uint32_t width, height;
    float reflections, padding;
};

struct BlendParameters
{
    float reflections, padding[3];
};

struct MaskParameters
{
    std::uint32_t tile, padding[3];
};

// The effect plan runs path tracing (configure), with DLSS Ray Reconstruction asked for, which it waits on;
// and how strongly the reflections are added.
struct Wanted
{
    bool on{};
    float reflections{};
};

FrameLatch<Wanted> latch;
std::atomic<bool> wanted{}, pipelineFailed{};

// The scene's depth each context's reflection mask read, for the blend it records next.
std::mutex depthMutex;
std::unordered_map<ID3D11DeviceContext*, ComPtr<ID3D11ShaderResourceView>> sceneDepths;

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> addShader, maskShader;
ComPtr<ID3D11VertexShader> fullScreenShader;
ComPtr<ID3D11PixelShader> blendShader, lampShader;
ComPtr<ID3D11Buffer> parameterBuffer, blendParameters, maskParameters;
ComPtr<ID3D11SamplerState> maskSampler;
ComPtr<ID3D11BlendState> added;
ComputeTexture specularWithReflections;
// The masks' atlas: GI_LAMP_MASK_TILES tiles across, shared with Direct3D 12.
ComputeTexture maskAtlas;

// Compiles the shaders once; call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (blendShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        parameterBuffer = makeConstantBuffer(device.Get(), sizeof(Parameters), "Creating the reflections' constants");
        addShader = computeShader(device.Get(), shaders::AddReflections, "pt-add-reflections");
        blendParameters = makeConstantBuffer(device.Get(), sizeof(BlendParameters), "Creating the reflections' blend constants");

        const auto tiles = std::to_string(GI_LAMP_MASK_TILES), size = std::to_string(GI_LAMP_MASK_SIZE);
        const D3D_SHADER_MACRO maskDefines[]{{"GI_LAMP_MASK_TILES", tiles.c_str()}, {"GI_LAMP_MASK_SIZE", size.c_str()}, {}};
        maskShader = computeShader(device.Get(), shaders::MaskCopy, "pt-masks", maskDefines);
        maskParameters = makeConstantBuffer(device.Get(), sizeof(MaskParameters), "Creating the masks' constants");
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler, maskSampler.ReleaseAndGetAddressOf()), "Creating the masks' sampler");

        const auto fullScreenCode = compile(shaders::FullScreen, "pt-full-screen", "vs_5_0");
        check(device->CreateVertexShader(fullScreenCode->GetBufferPointer(), fullScreenCode->GetBufferSize(), nullptr, fullScreenShader.ReleaseAndGetAddressOf()),
            "Creating the lamps' vertex shader");
        lampShader = pixelShader(device.Get(), shaders::AddLampLight, "pt-add-lamp-light");
        // Added as the game adds its lights' light, its alpha (the lights' shadow share) kept.
        D3D11_BLEND_DESC adding{};
        adding.RenderTarget[0].BlendEnable = TRUE;
        adding.RenderTarget[0].SrcBlend = adding.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        adding.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        adding.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        adding.RenderTarget[0].BlendOp = adding.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        adding.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        check(device->CreateBlendState(&adding, added.ReleaseAndGetAddressOf()), "Creating the lamps' blend");

        // Last, so it marks a complete pipeline.
        blendShader = pixelShader(device.Get(), shaders::ReflectionBlend, "pt-reflection-blend");
        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("Path tracing is unavailable: ") + error.what());
        return false;
    }
}

// The masks' tiles into the atlas; call with resourceMutex held.
void copyMasks(ID3D11DeviceContext* immediate, const std::vector<ID3D11ShaderResourceView*>& masks)
{
    if (!maskAtlas.texture)
        maskAtlas = makeComputeTexture(device.Get(), GI_LAMP_MASK_TILES * GI_LAMP_MASK_SIZE, GI_LAMP_MASK_SIZE, DXGI_FORMAT_R8G8B8A8_UNORM,
            "Creating path tracing's mask atlas", SharedWithD3D12);

    const SavedComputeState saved(immediate, {.buffers = 1, .views = 1, .accesses = 1, .samplers = 1});
    immediate->CSSetShader(maskShader.Get(), nullptr, 0);
    immediate->CSSetConstantBuffers(0, 1, maskParameters.GetAddressOf());
    immediate->CSSetSamplers(0, 1, maskSampler.GetAddressOf());
    immediate->CSSetUnorderedAccessViews(0, 1, maskAtlas.access.GetAddressOf(), nullptr);
    for (std::uint32_t tile = 0; tile < masks.size(); ++tile)
    {
        check(write(immediate, maskParameters.Get(), MaskParameters{tile}), "Writing the masks' constants");
        immediate->CSSetShaderResources(0, 1, &masks[tile]);
        immediate->Dispatch(groups(GI_LAMP_MASK_SIZE), groups(GI_LAMP_MASK_SIZE), 1);
    }
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (wanted) ensurePipeline();
}

void detach() noexcept
{
    {
        std::lock_guard lock(depthMutex);
        sceneDepths.clear();
    }

    std::lock_guard lock(resourceMutex);
    addShader.Reset();
    maskShader.Reset();
    fullScreenShader.Reset();
    blendShader.Reset();
    lampShader.Reset();
    parameterBuffer.Reset();
    blendParameters.Reset();
    maskParameters.Reset();
    maskSampler.Reset();
    added.Reset();
    specularWithReflections = maskAtlas = {};
    device.Reset();
    pipelineFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    const bool on = configuration.effects.runs(Effect::PathTracing);
    latch.set({on, configuration.settings.pathTracing.reflections});
    wanted = on;
    if (!on) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

void present() noexcept
{
    latch.present();
}

bool isReflecting() noexcept
{
    return wanted.load(std::memory_order_relaxed) && latch.frame().on && ray_tracing::isPathTracing() && !sun_light_pass::failed();
}

float reflectionStrength() noexcept
{
    return latch.frame().reflections;
}

Health health()
{
    if (!wanted) return {};
    if (pipelineFailed) return Health::unavailable("Path tracing is unavailable (see PhantomFX.log).");
    if (sun_light_pass::failed()) return Health::unavailable("The sun light pass's shaders are unavailable (see PhantomFX.log).");
    if (!ray_tracing::isPathTracing()) return Health::waiting("Waits for DLSS Ray Reconstruction to denoise a frame.");

    return {};
}

ID3D11ShaderResourceView* addReflections(ID3D11DeviceContext* context, ID3D11ShaderResourceView* specular, std::pair<UINT, UINT> size)
{
    if (!isReflecting() || !specular || sizeOf(specular) != size) return nullptr;

    auto* reflection = ray_tracing::reflection(size.first, size.second);
    if (!reflection) return nullptr;

    try
    {
        std::lock_guard lock(resourceMutex);
        if (!ensurePipeline()) return nullptr;

        const auto [width, height] = size;
        if (specularWithReflections.width != width || specularWithReflections.height != height)
            specularWithReflections = makeComputeTexture(device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the specular light with reflections");

        check(write(context, parameterBuffer.Get(), Parameters{width, height, latch.frame().reflections}), "Writing the reflections' constants");
        const performance::Timed timed(context, performance::Pass::PathTracedLight);
        const SavedComputeState saved(context, PassSlots);
        ID3D11ShaderResourceView* inputs[]{specular, reflection};
        auto* output = specularWithReflections.access.Get();
        context->CSSetConstantBuffers(0, 1, parameterBuffer.GetAddressOf());
        context->CSSetShaderResources(0, 2, inputs);
        context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        context->CSSetShader(addShader.Get(), nullptr, 0);
        context->Dispatch(groups(width), groups(height), 1);
        logOnce("Path tracing: the resolve adds the traced reflections.");

        return specularWithReflections.view.Get();
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("Path tracing: the resolve keeps the game's specular light: ") + error.what());
        return nullptr;
    }
}

void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!wanted.load(std::memory_order_relaxed) || call.indexed || call.count != 3 || call.instances != 1) return;

    try
    {
        const auto* row = shader.row();
        if (!row || row->pass != GamePass::ReflectionMask) return;

        ComPtr<ID3D11ShaderResourceView> depth;
        context->PSGetShaderResources(MaskDepthSlot, 1, depth.GetAddressOf());
        std::lock_guard lock(depthMutex);
        sceneDepths[context] = std::move(depth);
    }
    catch (...)
    {
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!wanted.load(std::memory_order_relaxed) || call.indexed || call.count != 3 || call.instances != 1) return false;

    try
    {
        const auto* row = shader.row();
        if (!row || row->pass != GamePass::ReflectionAdd || !isReflecting()) return false;

        ComPtr<ID3D11RenderTargetView> target;
        context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        const auto [width, height] = sizeOf(target.Get());
        const auto surfaceDepth = ray_tracing::surfaceDepth(width, height);
        auto* reflectionMap = ray_tracing::reflectionMap(width, height);
        auto* reflection = ray_tracing::reflection(width, height);
        if (!surfaceDepth || !reflectionMap || !reflection) return false;

        const auto surfaceWeights = glass::surfaceWeights(width, height);

        // Without the mask's depth, the rays take every surface.
        ComPtr<ID3D11ShaderResourceView> sceneDepth;
        {
            std::lock_guard lock(depthMutex);
            if (const auto found = sceneDepths.find(context); found != sceneDepths.end()) sceneDepth = std::move(found->second);
        }

        std::lock_guard lock(resourceMutex);
        if (!ensurePipeline()) return false;

        check(write(context, blendParameters.Get(), BlendParameters{latch.frame().reflections}), "Writing the reflections' blend constants");
        ComPtr<ID3D11PixelShader> gameShader;
        ComPtr<ID3D11ShaderResourceView> gameViews[BlendViews];
        ComPtr<ID3D11Buffer> gameParameters;
        context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
        context->PSGetConstantBuffers(BlendParameterSlot, 1, gameParameters.GetAddressOf());
        for (UINT i = 0; i < BlendViews; ++i) context->PSGetShaderResources(BlendViewSlot + i, 1, gameViews[i].GetAddressOf());

        ID3D11ShaderResourceView* views[BlendViews]{sceneDepth.Get(), surfaceDepth.Get(), reflectionMap, reflection, surfaceWeights.Get()};
        context->PSSetShader(blendShader.Get(), nullptr, 0);
        context->PSSetConstantBuffers(BlendParameterSlot, 1, blendParameters.GetAddressOf());
        context->PSSetShaderResources(BlendViewSlot, BlendViews, views);
        context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);

        for (UINT i = 0; i < BlendViews; ++i) views[i] = gameViews[i].Get();
        context->PSSetShaderResources(BlendViewSlot, BlendViews, views);
        context->PSSetConstantBuffers(BlendParameterSlot, 1, gameParameters.GetAddressOf());
        context->PSSetShader(gameShader.Get(), nullptr, 0);
        logOnce(sceneDepth ? "Path tracing: the game's reflection blend takes the rays' hits, and the screen-space reflections on water."
                           : "Path tracing: the game's reflection blend takes the rays' hits.");

        return true;
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("Path tracing: the screen-space reflections stay the game's: ") + error.what());
        return false;
    }
}

ID3D11Texture2D* packMasks(ID3D11DeviceContext* immediate, std::span<local_lights::TracedLight> lights) noexcept
{
    try
    {
        // The atlas's masks in tile order, and each light's tile, from 1.
        std::vector<ID3D11ShaderResourceView*> masks;
        std::vector<ComPtr<ID3D11Resource>> textures;
        std::vector<std::uint32_t> tiles(lights.size());
        std::size_t unmasked{};
        for (std::size_t i = 0; i < lights.size(); ++i)
        {
            if (!lights[i].mask) continue;

            const auto texture = resourceOf(lights[i].mask.Get());
            const auto found = std::ranges::find(textures, texture);
            if (found != textures.end()) tiles[i] = static_cast<std::uint32_t>(found - textures.begin()) + 1;
            else if (masks.size() < GI_LAMP_MASK_TILES)
            {
                masks.push_back(lights[i].mask.Get());
                textures.push_back(texture);
                tiles[i] = static_cast<std::uint32_t>(masks.size());
            }
            else ++unmasked;
        }

        if (masks.empty()) return nullptr;

        if (unmasked)
            logOnce("Path tracing: more than " + std::to_string(GI_LAMP_MASK_TILES) + " masks in a frame; " + std::to_string(unmasked)
                + " masked spots past them are lit unmasked.");

        std::lock_guard lock(resourceMutex);
        if (!ensurePipeline()) return nullptr;

        copyMasks(immediate, masks);
        for (std::size_t i = 0; i < lights.size(); ++i) lights[i].light.maskTile = tiles[i];

        return maskAtlas.texture.Get();
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("Path tracing: the masked spots are lit unmasked: ") + error.what());
        return nullptr;
    }
}

bool addLampLight(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* diffuse, ID3D11RenderTargetView* specular) noexcept
{
    try
    {
        const auto [width, height] = sizeOf(diffuse);
        const auto lamps = ray_tracing::lampLight(width, height);
        if (!lamps) return false;

        std::lock_guard lock(resourceMutex);
        if (!ensurePipeline()) return false;

        const performance::Timed timed(immediate, performance::Pass::PathTracedLight);
        ID3D11RenderTargetView* targets[]{diffuse, specular};
        ID3D11ShaderResourceView* inputs[]{lamps->diffuse, lamps->specular};
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        immediate->IASetInputLayout(nullptr);
        immediate->VSSetShader(fullScreenShader.Get(), nullptr, 0);
        immediate->GSSetShader(nullptr, nullptr, 0);
        immediate->PSSetShader(lampShader.Get(), nullptr, 0);
        immediate->PSSetShaderResources(0, 2, inputs);
        immediate->OMSetRenderTargets(2, targets, nullptr);
        immediate->OMSetBlendState(added.Get(), nullptr, 0xFFFFFFFF);
        immediate->RSSetState(nullptr);
        immediate->RSSetViewports(1, &viewport);
        immediate->Draw(3, 0);
        ID3D11ShaderResourceView* unbound[2]{};
        immediate->PSSetShaderResources(0, 2, unbound);

        return true;
    }
    catch (...)
    {
        return false;
    }
}
}
