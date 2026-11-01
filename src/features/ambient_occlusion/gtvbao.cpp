#include "features/ambient_occlusion/gtvbao.hpp"

#include "d3d11_helpers.hpp"
#include "features/ambient_occlusion/gtvbao_shaders.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/indirect_light_pass.hpp"
#include "passes/visibility_bitmask.hpp"
#include "performance.hpp"

#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>

namespace phantom_fx::gtvbao
{
namespace
{
using Microsoft::WRL::ComPtr;
using visibility_bitmask::Target;

constexpr UINT SourceSlot = 0; // The upscale's inSourceBuffer, where shaders::Apply reads the visibility

FrameLatch<AmbientOcclusion> latch;
std::atomic<bool> wantedRunning{}, pipelineFailed{};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> applyShader;
ComPtr<ID3D11BlendState> multiplyBlend;
std::array<Target, 2> visibility;
Target edges; // Depth edges for the denoiser, written by the march
visibility_bitmask::Progress progress{"GTVBAO"};

// Compiles the shaders once; call with resourceMutex held. Returns false when GTVBAO is unavailable.
bool ensurePipeline()
{
    if (applyShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        // Multiplies the diffuse light by the visibility and leaves its alpha, and the specular light
        // the upscale also binds, as they are.
        D3D11_BLEND_DESC blend{};
        blend.IndependentBlendEnable = TRUE;
        auto& diffuse = blend.RenderTarget[0];
        diffuse.BlendEnable = TRUE;
        diffuse.SrcBlend = D3D11_BLEND_ZERO;
        diffuse.DestBlend = D3D11_BLEND_SRC_COLOR;
        diffuse.BlendOp = D3D11_BLEND_OP_ADD;
        diffuse.SrcBlendAlpha = D3D11_BLEND_ZERO;
        diffuse.DestBlendAlpha = D3D11_BLEND_ONE;
        diffuse.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        diffuse.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        check(device->CreateBlendState(&blend, multiplyBlend.ReleaseAndGetAddressOf()), "Creating the GTVBAO blend");

        // Last, so it marks a complete pipeline.
        applyShader = pixelShader(device.Get(), shaders::Apply, "gtvbao-apply");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("GTVBAO unavailable: ") + error.what());

        return false;
    }
}

// The working textures at the render size; call with resourceMutex held.
void ensureTargets(UINT width, UINT height)
{
    bool made = visibility_bitmask::ensureTarget(device.Get(), edges, width, height, DXGI_FORMAT_R8_UNORM, "Creating the GTVBAO depth edges");
    for (auto& target : visibility)
        made |= visibility_bitmask::ensureTarget(device.Get(), target, width, height, DXGI_FORMAT_R8_UNORM, "Creating the GTVBAO visibility");
    if (made) log("GTVBAO at " + std::to_string(width) + "x" + std::to_string(height) + ".");
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (wantedRunning) ensurePipeline();
}

void detach() noexcept
{
    std::lock_guard lock(resourceMutex);
    applyShader.Reset();
    multiplyBlend.Reset();
    visibility = {};
    edges = {};
    device.Reset();
    pipelineFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    const bool running = configuration.effects.runs(Effect::AmbientOcclusion);
    latch.set(configuration.settings.ambientOcclusion);
    wantedRunning = running;
    if (!running) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

Health health()
{
    if (!wantedRunning) return {};

    std::lock_guard lock(resourceMutex);
    return progress.health(pipelineFailed || visibility_bitmask::failed() || indirect_light_pass::failed());
}

void present() noexcept
{
    latch.present();
}

bool compute(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* normals, ID3D11ShaderResourceView* specular,
    ID3D11RenderTargetView* light, ID3D11Buffer* scene)
{
    const auto settings = latch.frame();

    D3D11_BUFFER_DESC sceneDesc{};
    if (scene) scene->GetDesc(&sceneDesc);

    const auto size = sizeOf(depth);
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline() || !visibility_bitmask::ready()) return false;
    if (!normals) return progress.wait("the sky light pass bound no normals before the upscale");
    if (!size.first || sizeOf(normals) != size || sizeOf(light) != size) return progress.wait("the lighting textures differ in size");
    if (sceneDesc.ByteWidth != visibility_bitmask::SceneConstantsSize) return progress.wait("the upscale has no scene constants");

    const auto [width, height] = size;
    ensureTargets(width, height);
    const visibility_bitmask::Parameters parameters{{width, height}, settings.radiusMetres, visibility_bitmask::MaxRadiusShare * static_cast<float>(height),
        settings.strength, 1, 1, 0};
    const SavedComputeState saved(context, visibility_bitmask::PassSlots);
    visibility_bitmask::march(context, parameters, scene, depth, normals, specular, visibility, edges);

    return true;
}

void apply(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    ComPtr<ID3D11PixelShader> gameShader;
    ComPtr<ID3D11ShaderResourceView> gameSource;
    ComPtr<ID3D11BlendState> gameBlend;
    float blendFactor[4]{};
    UINT sampleMask{};
    context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->PSGetShaderResources(SourceSlot, 1, gameSource.GetAddressOf());
    context->OMGetBlendState(gameBlend.GetAddressOf(), blendFactor, &sampleMask);

    std::lock_guard lock(resourceMutex);
    context->PSSetShader(applyShader.Get(), nullptr, 0);
    context->PSSetShaderResources(SourceSlot, 1, visibility[visibility_bitmask::Result].view.GetAddressOf());
    context->OMSetBlendState(multiplyBlend.Get(), nullptr, 0xFFFFFFFF);
    performance::Timed timed(context, performance::Pass::OcclusionApply);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    timed.end();

    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetShaderResources(SourceSlot, 1, gameSource.GetAddressOf());
    context->OMSetBlendState(gameBlend.Get(), blendFactor, sampleMask);
    progress.ran();
}

ID3D11ShaderResourceView* occlusion() noexcept
{
    std::lock_guard lock(resourceMutex);
    return visibility[visibility_bitmask::Result].view.Get();
}
}
