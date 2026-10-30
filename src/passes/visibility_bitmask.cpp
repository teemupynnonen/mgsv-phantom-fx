#include "passes/visibility_bitmask.hpp"

#include "log.hpp"
#include "passes/visibility_bitmask_shaders.hpp"
#include "performance.hpp"

#include <Windows.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace phantom_fx::visibility_bitmask
{
namespace
{
using Microsoft::WRL::ComPtr;

// shaders::PrefilterDepths writes the distances and four smaller copies (DistanceMipCount), each
// 8 x 8 group 16 x 16 pixels.
constexpr UINT PrefilterPixels = 16;

std::atomic<bool> wantedRunning{}, pipelineFailed{};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> prefilterShader, occlusionShader, denoiseShader, finalDenoiseShader;
ComPtr<ID3D11ComputeShader> lightShader, lightDenoiseShader, finalLightDenoiseShader;
ComPtr<ID3D11Buffer> parameterBuffer;
DistanceMips distances; // shaders::PrefilterDepths' output, written before each march

// Compiles the shaders once; call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (occlusionShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        const D3D_SHADER_MACRO occlusionPass[]{{"LIGHT", "0"}, {"FINAL", "0"}, {}}, lastOcclusionPass[]{{"LIGHT", "0"}, {"FINAL", "1"}, {}};
        const D3D_SHADER_MACRO lightPass[]{{"LIGHT", "1"}, {"FINAL", "0"}, {}}, lastLightPass[]{{"LIGHT", "1"}, {"FINAL", "1"}, {}};
        prefilterShader = computeShader(device.Get(), shaders::PrefilterDepths, "depth-prefilter");
        denoiseShader = computeShader(device.Get(), shaders::Denoise, "gtvbao-denoise", occlusionPass);
        finalDenoiseShader = computeShader(device.Get(), shaders::Denoise, "gtvbao-denoise-final", lastOcclusionPass);
        lightShader = computeShader(device.Get(), shaders::March, "ssilvb", lightPass);
        lightDenoiseShader = computeShader(device.Get(), shaders::Denoise, "ssilvb-denoise", lightPass);
        finalLightDenoiseShader = computeShader(device.Get(), shaders::Denoise, "ssilvb-denoise-final", lastLightPass);
        parameterBuffer = makeConstantBuffer(device.Get(), sizeof(Parameters), "Creating the march constants");

        // Last, so it marks a complete pipeline.
        occlusionShader = computeShader(device.Get(), shaders::March, "gtvbao", occlusionPass);
        log("Visibility bitmask march ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("GTVBAO and SSILVB unavailable: ") + error.what());

        return false;
    }
}

// The prefiltered distances at the render size into `mips`; call with resourceMutex held.
void ensureDistances(UINT width, UINT height, DistanceMips& mips)
{
    if (mips.texture && mips.width == width && mips.height == height) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = DistanceMipCount;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    DistanceMips made{.width = width, .height = height};
    check(device->CreateTexture2D(&desc, nullptr, made.texture.GetAddressOf()), "Creating the prefiltered distances");
    check(device->CreateShaderResourceView(made.texture.Get(), nullptr, made.view.GetAddressOf()), "Creating the prefiltered distances");
    for (UINT mip = 0; mip < DistanceMipCount; ++mip)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC access{};
        access.Format = desc.Format;
        access.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        access.Texture2D.MipSlice = mip;
        check(device->CreateUnorderedAccessView(made.texture.Get(), &access, made.access[mip].GetAddressOf()),
            "Creating the prefiltered distances");
    }

    mips = std::move(made);
}

// Records shaders::PrefilterDepths into `mips`, with the march's constants bound, and unbinds
// them for the caller to read.
void prefilterDepths(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* specular, DistanceMips& mips)
{
    ID3D11ShaderResourceView* inputs[]{depth, specular};
    ID3D11UnorderedAccessView* outputs[DistanceMipCount]{};
    for (UINT mip = 0; mip < DistanceMipCount; ++mip) outputs[mip] = mips.access[mip].Get();
    context->CSSetShaderResources(0, 2, inputs);
    context->CSSetUnorderedAccessViews(0, DistanceMipCount, outputs, nullptr);
    context->CSSetShader(prefilterShader.Get(), nullptr, 0);
    context->Dispatch((mips.width + PrefilterPixels - 1) / PrefilterPixels, (mips.height + PrefilterPixels - 1) / PrefilterPixels, 1);

    ID3D11UnorderedAccessView* none[DistanceMipCount]{};
    context->CSSetUnorderedAccessViews(0, DistanceMipCount, none, nullptr);
}

// Records the denoise passes over what a march wrote into sources[0] and `edges`, alternating
// between the two sources; the last pass writes sources[Result].
void denoise(ID3D11DeviceContext* context, const std::array<Target, 2>& sources, const Target& edges, ID3D11ComputeShader* shader,
    ID3D11ComputeShader* finalShader)
{
    for (int pass = 0; pass < DenoisePasses; ++pass)
    {
        const bool last = pass + 1 == DenoisePasses;
        const auto& source = sources[pass % 2];
        auto* target = sources[1 - pass % 2].access.Get();
        // A texture still bound as an output would be refused as an input.
        ID3D11UnorderedAccessView* none[2]{};
        ID3D11ShaderResourceView* inputs[]{source.view.Get(), edges.view.Get()};
        context->CSSetUnorderedAccessViews(0, 2, none, nullptr);
        context->CSSetShaderResources(0, 2, inputs);
        context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        context->CSSetShader(last ? finalShader : shader, nullptr, 0);
        context->Dispatch(groups(source.width), groups(source.height), 1);
    }
}
}

bool ensureTarget(ID3D11Device* targetDevice, Target& target, UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    if (target.texture && target.width == width && target.height == height) return false;

    target = makeComputeTexture(targetDevice, width, height, format, what);

    return true;
}

bool Progress::wait(std::string reason)
{
    logOnce(std::string(name) + " waiting: " + reason + ".");
    waitingFor = std::move(reason);
    return false;
}

void Progress::ran()
{
    lastRun = GetTickCount64();
    waitingFor.clear();
}

Health Progress::health(bool failed) const
{
    const std::string effect = name;
    if (failed) return Health::unavailable(effect + " is unavailable (see PhantomFX.log).");
    if (lastRun && GetTickCount64() - lastRun < 1000) return {};

    return Health::waiting(waitingFor.empty() ? effect + " is waiting for the game's 3D view." : effect + " is waiting: " + waitingFor + ".");
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
    prefilterShader.Reset();
    occlusionShader.Reset();
    denoiseShader.Reset();
    finalDenoiseShader.Reset();
    lightShader.Reset();
    lightDenoiseShader.Reset();
    finalLightDenoiseShader.Reset();
    parameterBuffer.Reset();
    distances = {};
    device.Reset();
    pipelineFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    const bool running = configuration.effects.runs(Effect::AmbientOcclusion) || configuration.effects.runs(Effect::IndirectLight);
    wantedRunning = running;
    if (!running) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

bool ready() noexcept
{
    std::lock_guard lock(resourceMutex);
    return ensurePipeline();
}

bool failed() noexcept
{
    return pipelineFailed;
}

void march(ID3D11DeviceContext* context, Mode mode, const Parameters& parameters, ID3D11Buffer* scene, ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* normals, ID3D11ShaderResourceView* diffuse, ID3D11ShaderResourceView* albedo, ID3D11ShaderResourceView* specular,
    std::array<Target, 2>& results, Target& edges)
{
    const bool light = mode == Mode::Light;
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) throw std::runtime_error("The visibility bitmask march is unavailable.");

    ensureDistances(parameters.size[0], parameters.size[1], distances);
    check(write(context, parameterBuffer.Get(), parameters), "Writing the march constants");

    ID3D11Buffer* buffers[]{parameterBuffer.Get(), scene};
    context->CSSetConstantBuffers(0, 2, buffers);
    performance::Timed timed(context, light ? performance::Pass::LightPrefilter : performance::Pass::OcclusionPrefilter);
    prefilterDepths(context, depth, specular, distances);

    timed.next(light ? performance::Pass::LightMarch : performance::Pass::OcclusionMarch);
    ID3D11ShaderResourceView* inputs[]{depth, normals, diffuse, albedo, distances.view.Get()};
    ID3D11UnorderedAccessView* outputs[]{results[0].access.Get(), edges.access.Get()};
    context->CSSetShaderResources(0, 5, inputs);
    context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
    context->CSSetShader(light ? lightShader.Get() : occlusionShader.Get(), nullptr, 0);
    context->Dispatch(groups(results[0].width), groups(results[0].height), 1);
    timed.next(light ? performance::Pass::LightDenoise : performance::Pass::OcclusionDenoise);
    denoise(context, results, edges, light ? lightDenoiseShader.Get() : denoiseShader.Get(), light ? finalLightDenoiseShader.Get() : finalDenoiseShader.Get());
}

void prefilterDistances(ID3D11DeviceContext* context, const Parameters& parameters, ID3D11Buffer* scene, ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* specular, DistanceMips& mips)
{
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) throw std::runtime_error("The visibility bitmask march is unavailable.");

    ensureDistances(parameters.size[0], parameters.size[1], mips);
    check(write(context, parameterBuffer.Get(), parameters), "Writing the march constants");

    ID3D11Buffer* buffers[]{parameterBuffer.Get(), scene};
    context->CSSetConstantBuffers(0, 2, buffers);
    performance::Timed timed(context, performance::Pass::OcclusionPrefilter);
    prefilterDepths(context, depth, specular, mips);
}
}
