#include "passes/contact_shadow_trace.hpp"

#include "constant_capture.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/contact_shadow_trace_shaders.hpp"

#include <bend_sss/bend_sss_cpu.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace phantom_fx::contact_shadow_trace
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_constants::SceneConstants;

constexpr int WaveSize = 64; // WAVE_SIZE in shaders::BendShadow

// BendShadow's constant buffer.
struct Parameters
{
    float lightCoordinate[4];
    std::int32_t waveOffset[2];
    float inverseDepthSize[2];
    float surfaceThickness, bilinearThreshold, shadowContrast, reach;
    float depthBounds[2];
    std::int32_t boundsMin[2], boundsMax[2];
    float projection[2]; // B, A: view Z from the reversed depth
    float fade[2];       // Metres: start and end of the distance fade
    float padding[2];
};
static_assert(sizeof(Parameters) % 16 == 0);

// Bend's recommended starting value, which suits Fox Engine's reversed depth as it is.
constexpr float BilinearThreshold = 0.02f;
// The contact shadows are at full strength out to this share of the fade distance.
constexpr float FadeStart = 0.6f;

FrameLatch<ContactShadowShape> latch;
std::atomic<bool> wantedRunning{}, pipelineFailed{}, sunTraces{}, lampsTrace{};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> traceShader;
ComPtr<ID3D11SamplerState> borderSampler;
ComPtr<ID3D11Buffer> parameterBuffer;

// Compiles the trace once; call with resourceMutex held. Returns false when it is unavailable.
bool ensurePipeline()
{
    if (traceShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER; // Border colour 0: the far plane
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler, borderSampler.ReleaseAndGetAddressOf()), "Creating the shadow trace sampler");

        parameterBuffer = makeConstantBuffer(device.Get(), sizeof(Parameters), "Creating the shadow trace constants");

        // Last, so it marks a complete pipeline.
        traceShader = computeShader(device.Get(), shaders::BendShadow, "bend-screen-space-shadows");
        log("Screen-space shadow trace ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        traceShader.Reset();
        log(std::string("Screen-space shadows unavailable: ") + error.what());

        return false;
    }
}

// `target` at `width` x `height`; call with resourceMutex held.
void ensureShadowTarget(ShadowTarget& target, UINT width, UINT height)
{
    if (target.texture && target.width == width && target.height == height) return;

    // Command lists that still use the previous texture hold their own references.
    target = makeComputeTexture(device.Get(), width, height, DXGI_FORMAT_R8_UNORM, "Creating the screen-space shadow");
    log("Screen-space shadows at " + std::to_string(width) + "x" + std::to_string(height) + ".");
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
    traceShader.Reset();
    borderSampler.Reset();
    parameterBuffer.Reset();
    device.Reset();
    pipelineFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    const bool running = configuration.effects.runs(Effect::SunContactShadows) || configuration.effects.runs(Effect::LampContactShadows);
    latch.set(configuration.settings.contactShadows.shape);
    wantedRunning = running;
    if (!running) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

void present() noexcept
{
    latch.present();
}

ContactShadowShape shape() noexcept
{
    return latch.frame();
}

bool failed() noexcept
{
    return pipelineFailed;
}

void tracesThisFrame(Tracer tracer, bool traces) noexcept
{
    (tracer == Tracer::Sun ? sunTraces : lampsTrace) = traces;
    constant_capture::keepLightPasses(sunTraces || lampsTrace);
}

std::array<float, 4> project(const SceneConstants& scene, const float (&light)[4])
{
    const auto& projection = scene.projectionParam;
    const auto& offset = scene.cameraCenterOffset;

    return {light[0] / projection[0] + offset[0] * light[2], light[1] / projection[1] + offset[1] * light[2],
        projection[3] * light[2] + projection[2] * light[3], light[2]};
}

std::array<int, 2> depthSize(ID3D11ShaderResourceView* view)
{
    const auto [width, height] = sizeOf(view);
    if (!width) throw std::runtime_error("Reading the depth buffer failed: it is not a 2D texture.");

    return {static_cast<int>(width), static_cast<int>(height)};
}

std::optional<Traced> trace(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, const SceneConstants& scene, std::array<float, 4> lightProjection,
    ShadowTarget& target)
{
    auto size = depthSize(depth);
    const auto shape = latch.frame();
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return std::nullopt;

    ensureShadowTarget(target, static_cast<UINT>(size[0]), static_cast<UINT>(size[1]));

    // Bend's default depth bounds skip the sky.
    int boundsMin[2]{0, 0}, boundsMax[2]{size[0], size[1]};
    const auto dispatches = Bend::BuildDispatchList(lightProjection.data(), size.data(), boundsMin, boundsMax, false, WaveSize);

    const SavedComputeState saved(context, {.buffers = 1, .views = 1, .accesses = 1, .samplers = 1});

    const float lit[4]{1, 1, 1, 1};
    context->ClearUnorderedAccessViewFloat(target.access.Get(), lit);

    context->CSSetShader(traceShader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &depth);
    context->CSSetUnorderedAccessViews(0, 1, target.access.GetAddressOf(), nullptr);
    context->CSSetSamplers(0, 1, borderSampler.GetAddressOf());
    context->CSSetConstantBuffers(0, 1, parameterBuffer.GetAddressOf());
    for (int i = 0; i < dispatches.DispatchCount; ++i)
    {
        const auto& dispatch = dispatches.Dispatch[i];
        Parameters parameters{};
        std::memcpy(parameters.lightCoordinate, dispatches.LightCoordinate_Shader, sizeof(parameters.lightCoordinate));
        parameters.waveOffset[0] = dispatch.WaveOffset_Shader[0];
        parameters.waveOffset[1] = dispatch.WaveOffset_Shader[1];
        parameters.inverseDepthSize[0] = 1.0f / static_cast<float>(size[0]);
        parameters.inverseDepthSize[1] = 1.0f / static_cast<float>(size[1]);
        parameters.surfaceThickness = shape.thickness;
        parameters.bilinearThreshold = BilinearThreshold;
        parameters.shadowContrast = shape.contrast;
        parameters.reach = shape.reachPixels;
        // The light passes reconstruct the view depth as z = B / (depth - A).
        parameters.projection[0] = scene.projectionParam[2];
        parameters.projection[1] = scene.projectionParam[3];
        parameters.fade[0] = shape.fadeMetres * FadeStart;
        parameters.fade[1] = shape.fadeMetres;
        parameters.depthBounds[0] = 0;
        parameters.depthBounds[1] = 1;
        std::memcpy(parameters.boundsMin, boundsMin, sizeof(parameters.boundsMin));
        std::memcpy(parameters.boundsMax, boundsMax, sizeof(parameters.boundsMax));
        if (FAILED(write(context, parameterBuffer.Get(), parameters))) break;

        context->Dispatch(static_cast<UINT>(dispatch.WaveCount[0]), static_cast<UINT>(dispatch.WaveCount[1]), static_cast<UINT>(dispatch.WaveCount[2]));
    }

    return Traced{{dispatches.LightCoordinate_Shader[0], dispatches.LightCoordinate_Shader[1]}, dispatches.DispatchCount};
}

std::string describe(const Traced& traced)
{
    return "light at pixel " + std::to_string(traced.lightPixel[0]) + ", " + std::to_string(traced.lightPixel[1]) + ", "
        + std::to_string(traced.dispatches) + " dispatches";
}
}
