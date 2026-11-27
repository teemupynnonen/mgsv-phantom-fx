#include "features/indirect_light/ssgi.hpp"

#include "constant_capture.hpp"
#include "d3d11_helpers.hpp"
#include "features/motion_blur/motion_blur.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/indirect_light_pass.hpp"
#include "performance.hpp"
#include "ssgi_sources.h" // Generated from ssgi_*.hlsl (CMakeLists.txt)

#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace phantom_fx::ssgi
{
using fxcore::FrameLatch;

namespace
{
using Microsoft::WRL::ComPtr;
using visibility_bitmask::Target;

// The passes' constant buffer (the GI cbuffer in ssgi_common.hlsli), b0.
struct Parameters
{
    std::uint32_t size[2]; // c0.xy
    float radius;          // c0.z
    float amount;          // c0.w
    float samples;         // c1.x
    float scale;           // c1.y
    float filterStep;      // c1.z
    float historyBlend;    // c1.w
    float historyValid;    // c2.x
    float debug;           // c2.y
    float diffuseScale;    // c2.z
    float random;          // c2.w
    float minSamples;      // c3.x
    float visibility;      // c3.y
    float padding[2];      // c3.zw; the buffer rounds to 64 bytes
};

// A constant buffer's size must be a multiple of 16.
static_assert(sizeof(Parameters) == 64);

// The game's compute slots the passes bind, saved around them.
constexpr ComputeSlots PassSlots{.buffers = 2, .views = 8, .accesses = 8};

// VB-SSGI's settings, and whether the resolve shows its light alone (the diagnostics).
struct Wanted
{
    IndirectLight light;
    bool shownAlone{};
};

FrameLatch<Wanted> latch;
std::atomic<bool> wantedRunning{}, pipelineFailed{};
std::atomic<std::uint64_t> frameCounter{}; // For the gather's per-frame noise

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> gatherShader, accumulateShader, filterShader, resolveShader;
ComPtr<ID3D11Buffer> parameterBuffer;
Target fallback; // 1x1, for a binding a pass must have but does not read
Target bounce;   // The gather's output, at the gather's size
Target filtered, accumulated;
std::array<Target, 2> history;     // Ping-ponged accumulated history
std::array<Target, 2> historyAux;  // Depth, normal.xy, history length
std::array<Target, 2> moments;     // Luminance moments, RG16F
UINT historyIndex{};
UINT historyWidth{}, historyHeight{};
bool historyValid{};
// The prefiltered distances at the render size, for the gather's per-tap occlusion test; the march
// keeps its own, apart from this.
visibility_bitmask::DistanceMips mips;
visibility_bitmask::Progress progress{"VB-SSGI"};

// A pass: its file beside this source, its text as the build embedded it, and its name in the log.
struct PassSource
{
    const char* file;
    const char* embedded;
    const char* name;
};

constexpr PassSource PassSources[]{
    {"ssgi_gather.hlsl", shaders::gather, "ssgi-gather"},
    {"ssgi_accumulate.hlsl", shaders::accumulate, "ssgi-accumulate"},
    {"ssgi_filter.hlsl", shaders::filter, "ssgi-filter"},
    {"ssgi_resolve.hlsl", shaders::resolve, "ssgi-resolve"},
};

// Compiles one pass; call with resourceMutex held. A dev build reads the pass from its file in the
// source tree, so a saved edit reaches the game without a rebuild (its includes come from there too);
// a file that is not there, and every other build, takes the embedded text.
ComPtr<ID3D11ComputeShader> compilePass(const PassSource& pass)
{
#if PHANTOM_FX_DEV_TOOLS
    const auto path = std::filesystem::path(PHANTOM_FX_SSGI_SHADER_DIR) / pass.file;
    if (std::filesystem::exists(path))
    {
        ComPtr<ID3DBlob> code, errors;
        const auto result = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(result))
            throw std::runtime_error(std::string("Shader ") + pass.file + " did not compile: "
                + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                          : hex(static_cast<std::uint32_t>(result))));

        ComPtr<ID3D11ComputeShader> shader;
        check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf()), pass.name);

        return shader;
    }
#endif

    return computeShader(device.Get(), pass.embedded, pass.name);
}

// Compiles the four passes, in PassSources' order; call with resourceMutex held.
std::array<ComPtr<ID3D11ComputeShader>, std::size(PassSources)> compilePasses()
{
    std::array<ComPtr<ID3D11ComputeShader>, std::size(PassSources)> made;
    for (std::size_t i = 0; i < made.size(); ++i) made[i] = compilePass(PassSources[i]);

    return made;
}

void usePasses(const std::array<ComPtr<ID3D11ComputeShader>, std::size(PassSources)>& passes)
{
    gatherShader = passes[0];
    accumulateShader = passes[1];
    filterShader = passes[2];
    resolveShader = passes[3];
}

#if PHANTOM_FX_DEV_TOOLS
// A dev build's hot reload: present notes a pass's file saved, and the next recording compiles them
// all again. A failed compile is logged and the passes in use stay.
std::atomic<bool> reloadWanted{};
std::filesystem::file_time_type sourcesSeen{};
std::uint64_t sourcesChecked{};

// The newest save among the passes' files and their include.
std::filesystem::file_time_type newestSource() noexcept
{
    std::filesystem::file_time_type newest{};
    std::error_code ignored;
    for (const auto* file : {"ssgi_common.hlsli", PassSources[0].file, PassSources[1].file, PassSources[2].file, PassSources[3].file})
        newest = std::max(newest, std::filesystem::last_write_time(std::filesystem::path(PHANTOM_FX_SSGI_SHADER_DIR) / file, ignored));

    return newest;
}

// Called each present; looks at the files twice a second or so, which a frame can afford.
void watchSources() noexcept
{
    constexpr std::uint64_t Interval = 30;
    if (++sourcesChecked % Interval != 1) return;

    const auto newest = newestSource();
    if (sourcesSeen == std::filesystem::file_time_type{})
        sourcesSeen = newest;
    else if (newest != sourcesSeen)
    {
        sourcesSeen = newest;
        reloadWanted = true;
    }
}

// Call with resourceMutex held, with the pipeline made.
void reloadPasses()
{
    try
    {
        usePasses(compilePasses());
        historyValid = false; // The old passes' history is not the new ones'
        log("VB-SSGI shaders reloaded.");
    }
    catch (const std::exception& error)
    {
        log(std::string("VB-SSGI shader reload failed, the previous shaders stay: ") + error.what());
    }
}
#endif

// Compiles the shaders once; call with resourceMutex held. Returns false when VB-SSGI is unavailable.
bool ensurePipeline()
{
    // The last object made marks a complete pipeline, so a failure part way through is retried
    // rather than masked by the objects made before it.
    if (parameterBuffer) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        usePasses(compilePasses());
        fallback = makeComputeTexture(device.Get(), 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the VB-SSGI fallback");
        // Last, so it marks a complete pipeline.
        parameterBuffer = makeConstantBuffer(device.Get(), sizeof(Parameters), "Creating the VB-SSGI constants");
        log("VB-SSGI ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("VB-SSGI unavailable: ") + error.what());

        return false;
    }
}

// The passes' targets at the gather's size, and the lit diffuse light at the render size; call with
// resourceMutex held.
void ensureTargets(UINT gatherWidth, UINT gatherHeight, UINT width, UINT height, Target& lit)
{
    bool made = visibility_bitmask::ensureTarget(device.Get(), bounce, gatherWidth, gatherHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
        "Creating the VB-SSGI bounce");
    made |= visibility_bitmask::ensureTarget(device.Get(), filtered, gatherWidth, gatherHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
        "Creating the VB-SSGI filtered");
    made |= visibility_bitmask::ensureTarget(device.Get(), accumulated, gatherWidth, gatherHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
        "Creating the VB-SSGI accumulated");
    for (auto& target : history)
        made |= visibility_bitmask::ensureTarget(device.Get(), target, gatherWidth, gatherHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
            "Creating the VB-SSGI history");
    for (auto& target : historyAux)
        made |= visibility_bitmask::ensureTarget(device.Get(), target, gatherWidth, gatherHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
            "Creating the VB-SSGI history aux");
    for (auto& target : moments)
        made |= visibility_bitmask::ensureTarget(device.Get(), target, gatherWidth, gatherHeight, DXGI_FORMAT_R16G16_FLOAT,
            "Creating the VB-SSGI moments");
    // The game's diffuse light's format.
    made |= visibility_bitmask::ensureTarget(device.Get(), lit, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the VB-SSGI diffuse light");
    if (made)
        log("VB-SSGI at " + std::to_string(gatherWidth) + "x" + std::to_string(gatherHeight) + " for " + std::to_string(width) + "x"
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
    gatherShader.Reset();
    accumulateShader.Reset();
    filterShader.Reset();
    resolveShader.Reset();
    parameterBuffer.Reset();
    fallback = {};
    bounce = {};
    filtered = {};
    accumulated = {};
    history = {};
    historyAux = {};
    moments = {};
    historyIndex = 0;
    historyWidth = historyHeight = 0;
    historyValid = false;
    mips = {};
    device.Reset();
    pipelineFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    const bool running = configuration.effects.runs(Effect::IndirectLight);
    latch.set({configuration.settings.indirectLight, configuration.diagnostics.showIndirectLight});
    wantedRunning = running;
    // The temporal accumulation reprojects with the game's velocity buffer, which the engine draws
    // only with motion blur or an upscaler; force the velocity passes on while SSGI runs.
    motion_blur::wantVelocityPasses(running);
    if (!running) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

Health health()
{
    if (!wantedRunning) return {};

    std::lock_guard lock(resourceMutex);
    return progress.health(pipelineFailed || indirect_light_pass::failed());
}

void present() noexcept
{
    latch.present();
    ++frameCounter;
#if PHANTOM_FX_DEV_TOOLS
    watchSources();
#endif
}

bool compute(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* normals, ID3D11ShaderResourceView* diffuse,
    ID3D11ShaderResourceView* albedo, ID3D11ShaderResourceView* specular, ID3D11ShaderResourceView* specularMaterial,
    ID3D11ShaderResourceView* velocity, ID3D11Buffer* scene, Target& lit)
{
    const auto settings = latch.frame();

    D3D11_BUFFER_DESC sceneDesc{};
    if (scene) scene->GetDesc(&sceneDesc);

    const auto size = sizeOf(depth);
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;
#if PHANTOM_FX_DEV_TOOLS
    if (reloadWanted.exchange(false)) reloadPasses();
#endif
    if (!normals) return progress.wait("no sky light pass has bound the normals yet");
    if (!size.first || sizeOf(normals) != size || sizeOf(diffuse) != size || sizeOf(albedo) != size)
        return progress.wait("the lighting textures differ in size");
    if (sceneDesc.ByteWidth != visibility_bitmask::SceneConstantsSize) return progress.wait("the resolve has no scene constants");

    const auto [width, height] = size;
    const auto& light = settings.light;
    // 0 = full, 1 = half, 2 = quarter: the gather, history, moments and a-trous all follow.
    const UINT scale = light.resolution >= 2 ? 4u : (light.resolution == 1 ? 2u : 1u);
    const UINT gatherWidth = (width + scale - 1u) / scale, gatherHeight = (height + scale - 1u) / scale;

    // The history is only valid while its size matches and the chain has run continuously.
    if (historyWidth != gatherWidth || historyHeight != gatherHeight)
    {
        historyWidth = gatherWidth;
        historyHeight = gatherHeight;
        historyValid = false;
    }
    ensureTargets(gatherWidth, gatherHeight, width, height, lit);

    // Without this frame's velocity the blend collapses to zero: the raw gather, no accumulation.
    const bool velocityOk = velocity && sizeOf(velocity) == size;

    Parameters parameters{};
    parameters.size[0] = width;
    parameters.size[1] = height;
    parameters.radius = light.radius;
    parameters.amount = light.strength;
    parameters.samples = light.samples;
    parameters.scale = static_cast<float>(scale);
    parameters.historyBlend = light.history;
    parameters.historyValid = (historyValid && velocityOk) ? 1.0f : 0.0f;
    parameters.debug = static_cast<float>(light.debug);
    // The diagnostics and every debug view show SSGI's light alone, without the game's diffuse.
    parameters.diffuseScale = (settings.shownAlone || light.debug >= 1) ? 0.0f : 1.0f;
    parameters.random = static_cast<float>(frameCounter.load() & 0xFFFFu);
    parameters.minSamples = 1.0f;
    parameters.visibility = light.visibility;

    const SavedComputeState saved(context, PassSlots);

    // The prefiltered distances for the gather's per-tap occlusion test. Without the march's
    // pipeline the test is off and the gather binds the fallback.
    const visibility_bitmask::DistanceMips* distanceMips = nullptr;
    if (parameters.visibility > 0.0f && visibility_bitmask::ready())
    {
        visibility_bitmask::Parameters vbParameters{};
        vbParameters.size[0] = width;
        vbParameters.size[1] = height;
        // The mips' falloff radius: the gather's world radius at a nominal 50 m, from the projection.
        float invP11 = 1.0f / 3.1f;
        if (scene)
            if (const auto sceneConstants = constant_capture::written<game_constants::SceneConstants>(scene))
                invP11 = std::max(sceneConstants->projectionParam[1], 1e-3f);
        vbParameters.radius = light.radius * 2.0f * 50.0f * invP11;
        vbParameters.maxRadiusPixels = visibility_bitmask::MaxRadiusShare * static_cast<float>(height);
        try
        {
            visibility_bitmask::prefilterDistances(context, vbParameters, scene, depth, specularMaterial, mips);
            distanceMips = &mips;
        }
        catch (const std::exception& error)
        {
            log(std::string("VB-SSGI: the distance prefilter failed, the occlusion test is off: ") + error.what());
            parameters.visibility = 0.0f;
        }
    }
    else
    {
        parameters.visibility = 0.0f;
    }

    ID3D11Buffer* buffers[]{parameterBuffer.Get(), scene};
    context->CSSetConstantBuffers(0, 2, buffers);

    // One pass: clear the outputs (so no input reads what a previous pass wrote), bind, dispatch.
    const auto run = [&](ID3D11ComputeShader* shader, const std::vector<ID3D11ShaderResourceView*>& views,
                         const std::vector<ID3D11UnorderedAccessView*>& outputs, UINT w, UINT h) {
        ID3D11UnorderedAccessView* none[8]{};
        context->CSSetUnorderedAccessViews(0, 8, none, nullptr);
        context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
        context->CSSetShader(shader, nullptr, 0);
        context->Dispatch(groups(w), groups(h), 1);
    };
    const auto constants = [&](const Parameters& value) { check(write(context, parameterBuffer.Get(), value), "Writing the SSGI constants"); };

    performance::Timed timed(context, performance::Pass::SsgiGather);

    // 1. The gather samples the game's diffuse light and writes the bounce target.
    constants(parameters);
    run(gatherShader.Get(),
        {depth, normals, diffuse, albedo, specular ? specular : fallback.view.Get(),
            distanceMips ? distanceMips->view.Get() : fallback.view.Get()},
        {bounce.access.Get()}, gatherWidth, gatherHeight);

    // 2. Temporal accumulation against the previous result.
    timed.next(performance::Pass::SsgiAccumulate);
    const UINT writeIndex = historyIndex ^ 1u;
    run(accumulateShader.Get(),
        {bounce.view.Get(), history[historyIndex].view.Get(), velocityOk ? velocity : fallback.view.Get(), depth, normals,
            historyAux[historyIndex].view.Get(), moments[historyIndex].view.Get()},
        {history[writeIndex].access.Get(), accumulated.access.Get(), historyAux[writeIndex].access.Get(), moments[writeIndex].access.Get()},
        gatherWidth, gatherHeight);
    historyIndex = writeIndex;
    historyValid = true;

    // 3. A-trous: stride 1, 2, 4 per iteration. The targets alternate so no dispatch reads what it
    // writes, and the bounce target stays the gather.
    timed.next(performance::Pass::SsgiFilter);
    const UINT filterPasses = static_cast<UINT>(std::clamp(static_cast<int>(light.denoisePasses), 1, 3));
    ID3D11ShaderResourceView* filterInput = accumulated.view.Get();
    for (UINT step = 0; step < filterPasses; ++step)
    {
        parameters.filterStep = static_cast<float>(step);
        constants(parameters);
        const bool even = (step % 2) == 0;
        run(filterShader.Get(), {filterInput, depth, normals, moments[historyIndex].view.Get()},
            {even ? filtered.access.Get() : accumulated.access.Get()}, gatherWidth, gatherHeight);
        filterInput = even ? filtered.view.Get() : accumulated.view.Get();
    }
    parameters.filterStep = 0.0f;

    // An even pass count ends in the accumulated target, so the resolve follows whichever target
    // holds the last iteration instead of assuming it is the filtered one.
    const bool filteredInFiltered = (filterPasses % 2) != 0;
    const auto filteredView = filteredInFiltered ? filtered.view.Get() : accumulated.view.Get();

    // 4. The resolve, at the render size: the game's diffuse light with the bounce added.
    timed.next(performance::Pass::SsgiResolve);
    constants(parameters);
    run(resolveShader.Get(), {filteredView, depth, diffuse}, {lit.access.Get()}, width, height);

    progress.ran();

    return true;
}
}  // namespace phantom_fx::ssgi
