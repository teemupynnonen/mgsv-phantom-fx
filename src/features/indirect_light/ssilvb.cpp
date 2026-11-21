#include "features/indirect_light/ssilvb.hpp"

#include "d3d11_helpers.hpp"
#include "features/indirect_light/ssilvb_shaders.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/indirect_light_pass.hpp"
#include "performance.hpp"

#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>

namespace phantom_fx::ssilvb
{
using fxcore::FrameLatch;
namespace
{
using Microsoft::WRL::ComPtr;
using visibility_bitmask::Target;

// The march and the denoiser run at the render size over this, rounded up (Scale in shaders::March).
constexpr UINT LightScale = 2;

// SSILVB's settings, and whether the resolve shows its light alone (the diagnostics).
struct Wanted
{
    IndirectLight light;
    bool shownAlone{};
};

FrameLatch<Wanted> latch;
std::atomic<bool> wantedRunning{}, pipelineFailed{};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> upsampleShader;
std::array<Target, 2> gathered; // At the march's size
Target edges;                   // Depth edges for the denoiser, written by the march
visibility_bitmask::Progress progress{"SSILVB"};

// Compiles the shader once; call with resourceMutex held. Returns false when SSILVB is unavailable.
bool ensurePipeline()
{
    if (upsampleShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        upsampleShader = computeShader(device.Get(), shaders::Upsample, "ssilvb-upsample");
        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("SSILVB unavailable: ") + error.what());

        return false;
    }
}

// The march's textures at its size, and the lit diffuse light at the render size; call with
// resourceMutex held.
void ensureTargets(UINT width, UINT height, Target& lit)
{
    const UINT marchWidth = (width + LightScale - 1) / LightScale, marchHeight = (height + LightScale - 1) / LightScale;
    bool made = visibility_bitmask::ensureTarget(device.Get(), edges, marchWidth, marchHeight, DXGI_FORMAT_R8_UNORM, "Creating the SSILVB depth edges");
    for (auto& target : gathered)
        made |= visibility_bitmask::ensureTarget(device.Get(), target, marchWidth, marchHeight, DXGI_FORMAT_R11G11B10_FLOAT, "Creating the SSILVB light");
    // The game's diffuse light's format.
    made |= visibility_bitmask::ensureTarget(device.Get(), lit, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the SSILVB diffuse light");
    if (made)
        log("SSILVB at " + std::to_string(marchWidth) + "x" + std::to_string(marchHeight) + " for " + std::to_string(width) + "x"
            + std::to_string(height) + ".");
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
    upsampleShader.Reset();
    gathered = {};
    edges = {};
    device.Reset();
    pipelineFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    const bool running = configuration.effects.runs(Effect::IndirectLight);
    latch.set({configuration.settings.indirectLight, configuration.diagnostics.showIndirectLight});
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

bool compute(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* normals, ID3D11ShaderResourceView* diffuse,
    ID3D11ShaderResourceView* albedo, ID3D11ShaderResourceView* specular, ID3D11Buffer* scene, Target& lit)
{
    const auto settings = latch.frame();

    D3D11_BUFFER_DESC sceneDesc{};
    if (scene) scene->GetDesc(&sceneDesc);

    const auto size = sizeOf(depth);
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline() || !visibility_bitmask::ready()) return false;
    if (!normals) return progress.wait("no sky light pass has bound the normals yet");
    if (!size.first || sizeOf(normals) != size || sizeOf(diffuse) != size || sizeOf(albedo) != size)
        return progress.wait("the lighting textures differ in size");
    if (sceneDesc.ByteWidth != visibility_bitmask::SceneConstantsSize) return progress.wait("the resolve has no scene constants");

    const auto [width, height] = size;
    ensureTargets(width, height, lit);
    const visibility_bitmask::Parameters parameters{{width, height}, settings.light.radiusMetres, visibility_bitmask::MaxRadiusShare * static_cast<float>(height),
        1, settings.light.strength, settings.shownAlone ? 0.0f : 1.0f, 0};
    const SavedComputeState saved(context, visibility_bitmask::PassSlots);
    visibility_bitmask::march(context, visibility_bitmask::Mode::Light, parameters, scene, depth, normals, diffuse, albedo, specular, gathered, edges);

    // The march's constants and the scene stay bound for the upsample. The denoiser left
    // gathered[Result] bound as an output, which would refuse it as an input.
    const performance::Timed timed(context, performance::Pass::LightUpsample);
    ID3D11UnorderedAccessView* none[2]{};
    ID3D11ShaderResourceView* inputs[]{gathered[visibility_bitmask::Result].view.Get(), depth, diffuse};
    auto* output = lit.access.Get();
    context->CSSetUnorderedAccessViews(0, 2, none, nullptr);
    context->CSSetShaderResources(0, 3, inputs);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(upsampleShader.Get(), nullptr, 0);
    context->Dispatch(groups(width), groups(height), 1);
    progress.ran();

    return true;
}
}
