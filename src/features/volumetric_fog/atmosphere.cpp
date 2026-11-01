#include "features/volumetric_fog/atmosphere.hpp"

#include "camera.hpp"
#include "d3d11_helpers.hpp"
#include "device_check.hpp"
#include "features/hdr/hdr.hpp"
#include "features/path_tracing/sky_capture.hpp"
#include "features/volumetric_fog/atmosphere_shaders.hpp"
#include "frame_latch.hpp"
#include "game_constants.hpp"
#include "log.hpp"
#include "performance.hpp"

#include <Windows.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace phantom_fx::atmosphere
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_constants::ObjectConstants, game_constants::SceneConstants, game_constants::SunLightConstants, game_constants::SystemConstants;
using game_shaders::GamePass;

// The game's fog texture: 128 depth slices in 32 x 4 tiles of 16x64 texels. The add-on's tiles are
// finer across the view, for the edges of the light shafts: a froxel spans about 16 pixels at 4K.
constexpr UINT GameFogWidth = 512, GameFogHeight = 256;
constexpr UINT TileColumns = 32, TileRows = 4, Slices = 128;
constexpr UINT TileWidth = 240, TileHeight = 136;
constexpr UINT FogWidth = TileColumns * TileWidth, FogHeight = TileRows * TileHeight;
// Shadow comparisons per froxel and frame, spread through its depth, at a new place in the froxel
// each frame. The last frames' visibility, reprojected, keeps this share of the result, as in
// Frostbite (Hillaire 2015: one comparison, 5 % of the current frame).
constexpr UINT ShadowSamples = 2;
constexpr float HistoryWeight = 0.95f;
// The Halton sequence's period for the froxels' places.
constexpr std::uint32_t JitterPeriod = 64;
// Hillaire's lookup tables (atmosphere_shaders.hpp).
constexpr UINT TransmittanceWidth = 256, TransmittanceHeight = 64, MultipleScatteringSize = 32, SkyViewWidth = 192, SkyViewHeight = 108;
constexpr UINT SkyParameterTexels = 7;
// Milliseconds between the sky parameters in the log, while the physical look runs.
constexpr std::uint64_t SkyLogInterval = 10000;

// The fog pass draws two triangles per slice.
constexpr unsigned FogPassVertices = Slices * 6;

// The draws the fog and the sky work with: the fog pass's, and the other passes' full-screen triangles.
bool isFogOrFullScreen(const ui_redirect::DrawCall& call)
{
    return !call.indexed && call.instances == 1 && (call.count == 3 || call.count == FogPassVertices);
}
// Constant buffer slots and sizes of the game's passes: cPSSystem, cPSScene, the sun pass's cPSLight,
// cPSMaterial (the fog pass's and the sky's, whose registers nothing reads by name), cPSObject.
constexpr UINT SystemSlot = SystemConstants::Slot, SceneSlot = SceneConstants::Slot, LightSlot = SunLightConstants::Slot, MaterialSlot = 4,
               ObjectSlot = ObjectConstants::Slot;
constexpr UINT SystemSize = sizeof(SystemConstants), SceneSize = sizeof(SceneConstants), LightSize = sizeof(SunLightConstants), MaterialSize = 128,
               ObjectSize = sizeof(ObjectConstants);
constexpr UINT RainSlot = 0;   // The fog pass's inInterruptTexture
constexpr UINT ShadowSlot = 3; // The sun pass's g_ShadowTexture
constexpr UINT FogSlot = 12;   // g_tex_fog of every pass that reads the fog
// The sky draw's own textures: SkyParameters and SkyViewLut in atmosphere_shaders.hpp.
constexpr UINT SkyParameterSlot = 6, SkyViewSlot = 7;

// Fog in atmosphere_shaders.hpp.
struct Constants
{
    std::uint32_t tileSize[2], shadowed, shadowSamples;
    float jitter[2], depthJitter, historyWeight;
    std::uint32_t physical, skySun;
    float haze, padding;
};

// Copies of the game's constant buffers, made on the GPU in the command list that bound them, so the
// fill reads them as the game's passes did.
struct Copies
{
    ComPtr<ID3D11Buffer> system, scene, material, object; // The fog pass's
    ComPtr<ID3D11Buffer> sunScene, sunLight;              // The sun pass's
    ComPtr<ID3D11Buffer> lastSunScene;                    // The last fill's sunScene
    ComPtr<ID3D11Buffer> skyMaterial;                     // The last sky draw's cPSMaterial, whose sun lights the game's sky
    ComPtr<ID3D11Buffer> skyObject;                       // And its cPSObject: how overcast the game draws its sky
};

// A texture the passes write and read.
struct Target
{
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> access;
};

// The fog's settings, and whether the effect plan runs it.
struct Wanted
{
    VolumetricFog fog;
    bool running{};
};

FrameLatch<Wanted> latch;
std::atomic<bool> active{}, physicalLook{}, pipelineFailed{};
std::atomic<float> haze{};
std::atomic<std::uint64_t> lastFill{}; // GetTickCount64

// The game's fog texture, once made larger. The game owns it.
std::atomic<ID3D11Resource*> gameFog{};
thread_local bool creatingFog{};

// What the game's passes bound, for the fill.
std::mutex captureMutex;
ComPtr<ID3D11ShaderResourceView> rainView;
bool fogPassSeen{};
bool skySunSeen{}; // The last sky draw had the sun, and its constants were copied
// The shadow map each context's sun pass bound, for the resolve it records next in the same command
// list. The game records the next frame while it presents the last, so presents do not divide frames.
std::unordered_map<ID3D11DeviceContext*, ComPtr<ID3D11ShaderResourceView>> sunShadows;

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ID3D11DeviceContext* immediate{};
ComPtr<ID3D11ComputeShader> visibilityShader, mediaShader, integrateShader;
ComPtr<ID3D11ComputeShader> transmittanceShader, multipleScatteringShader, parameterShader, skyViewShader, ambientShader;
// The sky draw without and with the sun's or the moon's sprite.
std::array<ComPtr<ID3D11PixelShader>, 2> skyShaders;
ComPtr<ID3D11Buffer> constantBuffer;
ComPtr<ID3D11SamplerState> linearClamp, shadowSampler;
Copies copies;
Target media, fogOnly;
// The sun's share in each froxel: this frame's alone, and over frames, the last fill's and this one's.
Target currentVisibility;
std::array<Target, 2> visibility;
std::size_t visibilityWritten{};
Target transmittance, multipleScattering, skyParameters, skyView;
bool atmosphereTables{}; // The transmittance and multiple scattering tables are made
bool skyReady{};         // The last fill was physical and made the sky parameters and sky view
// The sky parameters on their way to the log, and when they last went.
ComPtr<ID3D11Texture2D> skyParameterReadback;
bool skyReadbackPending{};
std::uint64_t lastSkyLog{};
// Counts fills, to move the shadow comparisons; whether the last one had a shadow, whose visibility
// the next one may reproject.
std::uint32_t fills{};
bool lastFillShadowed{};
ComPtr<ID3D11UnorderedAccessView> fogAccess; // Of gameFog
ID3D11Resource* fogAccessOf{};

ComPtr<ID3D11Buffer> constantCopy(UINT size)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc, nullptr, buffer.GetAddressOf()), "Creating a constant buffer copy");

    return buffer;
}

Target target2d(UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    auto made = makeComputeTexture(device.Get(), width, height, format, what);
    return {std::move(made.view), std::move(made.access)};
}

// A froxel volume.
Target target3d(DXGI_FORMAT format, const char* what)
{
    D3D11_TEXTURE3D_DESC desc{};
    desc.Width = TileWidth;
    desc.Height = TileHeight;
    desc.Depth = Slices;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture3D> texture;
    Target made;
    check(device->CreateTexture3D(&desc, nullptr, texture.GetAddressOf()), what);
    check(device->CreateShaderResourceView(texture.Get(), nullptr, made.view.GetAddressOf()), what);
    check(device->CreateUnorderedAccessView(texture.Get(), nullptr, made.access.GetAddressOf()), what);

    return made;
}

// Compiles the shaders and makes the fixed resources once; call with resourceMutex held. Returns
// false when the volumetric fog is unavailable.
bool ensurePipeline()
{
    if (mediaShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        integrateShader = computeShader(device.Get(), shaders::FogIntegrate, "fog-integrate");
        visibilityShader = computeShader(device.Get(), shaders::FogVisibility, "fog-visibility");
        transmittanceShader = computeShader(device.Get(), shaders::AtmosphereTransmittance, "atmosphere-transmittance");
        multipleScatteringShader = computeShader(device.Get(), shaders::AtmosphereMultipleScattering, "atmosphere-multiple-scattering");
        parameterShader = computeShader(device.Get(), shaders::SkyParameterPass, "sky-parameters");
        skyViewShader = computeShader(device.Get(), shaders::SkyView, "sky-view");
        ambientShader = computeShader(device.Get(), shaders::SkyAmbient, "sky-ambient");
        // The float scene takes TppTonemap out of the scene for the session.
        const char* floatScene = hdr::isSceneFloat() ? "1" : "0";
        const char* hdrOutput = hdr::active() ? "1" : "0";
        const D3D_SHADER_MACRO withoutSprite[]{{"MOON", "0"}, {"FLOAT_SCENE", floatScene}, {"HDR_OUTPUT", hdrOutput}, {}};
        const D3D_SHADER_MACRO withSprite[]{{"MOON", "1"}, {"FLOAT_SCENE", floatScene}, {"HDR_OUTPUT", hdrOutput}, {}};
        skyShaders[0] = pixelShader(device.Get(), shaders::SkyDraw, "sky", withoutSprite);
        skyShaders[1] = pixelShader(device.Get(), shaders::SkyDraw, "sky-sprite", withSprite);

        constantBuffer = makeConstantBuffer(device.Get(), sizeof(Constants), "Creating the fog constants");
        copies = {constantCopy(SystemSize), constantCopy(SceneSize), constantCopy(MaterialSize), constantCopy(ObjectSize), constantCopy(SceneSize),
            constantCopy(LightSize), constantCopy(SceneSize), constantCopy(MaterialSize), constantCopy(ObjectSize)};

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler, linearClamp.ReleaseAndGetAddressOf()), "Creating the fog's linear sampler");
        // As the sun pass compares: 1 where the shadow map is further towards the sun than the point.
        sampler.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        sampler.ComparisonFunc = D3D11_COMPARISON_LESS;
        check(device->CreateSamplerState(&sampler, shadowSampler.ReleaseAndGetAddressOf()), "Creating the fog's shadow sampler");

        media = target3d(DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the fog media");
        fogOnly = target3d(DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the height fog media");
        currentVisibility = target3d(DXGI_FORMAT_R16_UNORM, "Creating the fog's visibility");
        for (auto& volume : visibility) volume = target3d(DXGI_FORMAT_R16_UNORM, "Creating the fog's visibility");
        transmittance = target2d(TransmittanceWidth, TransmittanceHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the transmittance table");
        multipleScattering = target2d(MultipleScatteringSize, MultipleScatteringSize, DXGI_FORMAT_R16G16B16A16_FLOAT,
            "Creating the multiple scattering table");
        skyParameters = target2d(SkyParameterTexels, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, "Creating the sky parameters");
        skyView = target2d(SkyViewWidth, SkyViewHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the sky view table");
        D3D11_TEXTURE2D_DESC readback{};
        readback.Width = SkyParameterTexels;
        readback.Height = readback.MipLevels = readback.ArraySize = 1;
        readback.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        readback.SampleDesc.Count = 1;
        readback.Usage = D3D11_USAGE_STAGING;
        readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&readback, nullptr, skyParameterReadback.ReleaseAndGetAddressOf()), "Creating the sky parameters' readback");

        // Last, so it marks a complete pipeline.
        mediaShader = computeShader(device.Get(), shaders::FogMedia, "fog-media");
        log("Volumetric fog ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("Volumetric fog unavailable: ") + error.what());

        return false;
    }
}

// The unordered access view of the game's fog texture; call with resourceMutex held.
ID3D11UnorderedAccessView* fogOutput(ID3D11Resource* fog)
{
    if (fogAccess && fogAccessOf == fog) return fogAccess.Get();

    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    check(device->CreateUnorderedAccessView(fog, &desc, fogAccess.ReleaseAndGetAddressOf()), "Creating the fog texture's output");
    fogAccessOf = fog;

    return fogAccess.Get();
}

// Copies the game's constant buffer `source` into `copy` on `context`, when it has the expected size.
bool copyConstants(ID3D11DeviceContext* context, ID3D11Buffer* copy, ID3D11Buffer* source, UINT size)
{
    D3D11_BUFFER_DESC desc{};
    if (source) source->GetDesc(&desc);
    if (!source || desc.ByteWidth != size) return false;

    context->CopyResource(copy, source);

    return true;
}

// Runs `shader` over `x` x `y` x `z` groups with `output` as its only unordered access, unbinding it after.
void dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, ID3D11UnorderedAccessView* output, UINT x, UINT y, UINT z)
{
    ID3D11UnorderedAccessView* const none[1]{};
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(shader, nullptr, 0);
    context->Dispatch(x, y, z);
    context->CSSetUnorderedAccessViews(0, 1, none, nullptr);
}

// The fog pass's constants and rain cover, for the fill.
void noteFogPass(ID3D11DeviceContext* context)
{
    std::array<ID3D11Buffer*, ObjectSlot + 1> buffers{};
    context->PSGetConstantBuffers(0, static_cast<UINT>(buffers.size()), buffers.data());
    ComPtr<ID3D11ShaderResourceView> rain;
    context->PSGetShaderResources(RainSlot, 1, rain.GetAddressOf());

    bool copied{};
    {
        std::lock_guard lock(resourceMutex);
        if (ensurePipeline())
        {
            copied = copyConstants(context, copies.system.Get(), buffers[SystemSlot], SystemSize)
                && copyConstants(context, copies.scene.Get(), buffers[SceneSlot], SceneSize)
                && copyConstants(context, copies.material.Get(), buffers[MaterialSlot], MaterialSize)
                && copyConstants(context, copies.object.Get(), buffers[ObjectSlot], ObjectSize);
            if (!copied) logOnce("Volumetric fog: the game's fog pass binds unexpected constants; the game's fog stays.");
        }
    }

    for (auto* buffer : buffers)
        if (buffer) buffer->Release();

    std::lock_guard lock(captureMutex);
    rainView = rain;
    fogPassSeen = copied;
}

// The game's fog pass, drawn over the whole of the larger texture: its geometry spans the texture
// in clip space, so each slice's tile takes the larger size.
bool drawFogPass(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, bool fog)
{
    ComPtr<ID3D11RenderTargetView> target;
    context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
    if (!target || resourceOf(target.Get()).Get() != gameFog.load()) return false;

    {
        const WholeTargetViewport whole(context, FogWidth, FogHeight);
        const performance::Timed timed(context, performance::Pass::FogPass);
        context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    }

    if (fog) noteFogPass(context);

    return true;
}

// The sun pass's shadow map and constants, for the fill.
void noteSun(ID3D11DeviceContext* context)
{
    std::array<ID3D11Buffer*, 2> buffers{};
    context->PSGetConstantBuffers(SceneSlot, static_cast<UINT>(buffers.size()), buffers.data());
    ComPtr<ID3D11ShaderResourceView> shadow;
    context->PSGetShaderResources(ShadowSlot, 1, shadow.GetAddressOf());

    bool copied{};
    {
        std::lock_guard lock(resourceMutex);
        copied = ensurePipeline() && copyConstants(context, copies.sunScene.Get(), buffers[0], SceneSize)
            && copyConstants(context, copies.sunLight.Get(), buffers[1], LightSize);
    }

    for (auto* buffer : buffers)
        if (buffer) buffer->Release();
    if (!copied || !shadow) return;

    std::lock_guard lock(captureMutex);
    sunShadows[context] = shadow;
}

// The physical look's sky: Hillaire's fixed tables once, then this frame's sky parameters, sky view
// and the sky's mean light. Call with resourceMutex held and the fill's bindings set.
void updateSky(ID3D11DeviceContext* context)
{
    const performance::Timed timed(context, performance::Pass::SkyTables);
    ID3D11ShaderResourceView* const none[4]{};
    if (!atmosphereTables)
    {
        dispatch(context, transmittanceShader.Get(), transmittance.access.Get(), groups(TransmittanceWidth), groups(TransmittanceHeight), 1);
        context->CSSetShaderResources(4, 1, transmittance.view.GetAddressOf());
        dispatch(context, multipleScatteringShader.Get(), multipleScattering.access.Get(), MultipleScatteringSize, MultipleScatteringSize, 1);
        atmosphereTables = true;
        log("Volumetric fog: made the atmosphere's transmittance and multiple scattering tables.");
    }

    ID3D11ShaderResourceView* const tables[]{transmittance.view.Get(), multipleScattering.view.Get()};
    context->CSSetShaderResources(4, static_cast<UINT>(std::size(tables)), tables);
    context->CSSetShaderResources(6, 2, none);
    dispatch(context, parameterShader.Get(), skyParameters.access.Get(), 1, 1, 1);
    context->CSSetShaderResources(6, 1, skyParameters.view.GetAddressOf());
    dispatch(context, skyViewShader.Get(), skyView.access.Get(), groups(SkyViewWidth), groups(SkyViewHeight), 1);
    context->CSSetShaderResources(6, 1, none);
    context->CSSetShaderResources(7, 1, skyView.view.GetAddressOf());
    dispatch(context, ambientShader.Get(), skyParameters.access.Get(), 1, 1, 1);
    context->CSSetShaderResources(6, 1, skyParameters.view.GetAddressOf());
    skyReady = true;
}

// Fills the game's fog texture with the shadowed fog, before the resolve that reads it first.
void fill(ID3D11DeviceContext* context, bool physical)
{
    auto* fog = gameFog.load();
    ComPtr<ID3D11ShaderResourceView> rain, shadow;
    bool shadowed{}, skySun{};
    {
        std::lock_guard lock(captureMutex);
        if (!fogPassSeen) return;

        rain = rainView;
        skySun = skySunSeen;
        if (const auto found = sunShadows.find(context); found != sunShadows.end())
        {
            shadow = std::move(found->second);
            sunShadows.erase(found);
        }
        shadowed = shadow != nullptr;
        // The physical look's sky needs the sun pass's constants, this frame's: a frame without a sun
        // pass (indoors) keeps the game's look. Until 2026-10-26 the last sun pass's were kept, and the
        // main menu's daytime sun lit the sky outside a night-time hospital.
        physical = physical && shadowed;
    }

    std::lock_guard lock(resourceMutex);
    if (!fog || !ensurePipeline()) return;

    auto* output = fogOutput(fog);
    // A new place in each froxel every frame, from the Halton sequence in bases 2 and 3 across the
    // view and 5 through the slice.
    const auto step = fills++ % JitterPeriod + 1;
    const float history = shadowed && lastFillShadowed ? HistoryWeight : 0;
    const Constants constants{{TileWidth, TileHeight}, shadowed ? 1u : 0u, ShadowSamples, {static_cast<float>(camera::halton(step, 2)) - 0.5f, static_cast<float>(camera::halton(step, 3)) - 0.5f},
        static_cast<float>(camera::halton(step, 5)), history, physical ? 1u : 0u, skySun ? 1u : 0u, haze.load(std::memory_order_relaxed), {}};
    check(write(context, constantBuffer.Get(), constants), "Writing the fog constants");

    // The resolve reads the fog texture; it cannot be bound for reading while the fill writes it.
    ComPtr<ID3D11ShaderResourceView> readers;
    context->PSGetShaderResources(FogSlot, 1, readers.GetAddressOf());
    ID3D11ShaderResourceView* const none[8]{};
    ID3D11UnorderedAccessView* const noAccess[3]{};
    context->PSSetShaderResources(FogSlot, 1, none);
    {
        SavedComputeState saved(context, {.buffers = 10, .views = 8, .accesses = 3, .samplers = 2});
        const auto last = visibilityWritten, next = 1 - visibilityWritten;
        ID3D11Buffer* const buffers[]{constantBuffer.Get(), copies.scene.Get(), copies.material.Get(), copies.object.Get(), copies.system.Get(),
            copies.sunScene.Get(), copies.sunLight.Get(), copies.lastSunScene.Get(), copies.skyMaterial.Get(), copies.skyObject.Get()};
        ID3D11SamplerState* const samplers[]{linearClamp.Get(), shadowSampler.Get()};
        context->CSSetConstantBuffers(0, static_cast<UINT>(std::size(buffers)), buffers);
        context->CSSetSamplers(0, static_cast<UINT>(std::size(samplers)), samplers);
        if (physical) updateSky(context);
        else skyReady = false;
        if (shadowed)
        {
            const performance::Timed timed(context, performance::Pass::FogShadow);
            context->CSSetShaderResources(1, 1, shadow.GetAddressOf());
            dispatch(context, visibilityShader.Get(), currentVisibility.access.Get(), groups(TileWidth), groups(TileHeight), Slices);
        }

        performance::Timed timed(context, performance::Pass::FogScattering);
        ID3D11ShaderResourceView* const inputs[]{rain.Get(), nullptr, visibility[last].view.Get(), currentVisibility.view.Get(),
            transmittance.view.Get(), multipleScattering.view.Get(), skyParameters.view.Get(), nullptr};
        ID3D11UnorderedAccessView* const outputs[]{media.access.Get(), visibility[next].access.Get(), fogOnly.access.Get()};
        context->CSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(outputs)), outputs, nullptr);
        context->CSSetShader(mediaShader.Get(), nullptr, 0);
        context->Dispatch(groups(TileWidth), groups(TileHeight), Slices);
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(noAccess)), noAccess, nullptr);

        timed.next(performance::Pass::FogIntegration);
        ID3D11ShaderResourceView* const sums[]{media.view.Get(), fogOnly.view.Get()};
        context->CSSetShaderResources(0, static_cast<UINT>(std::size(none)), none);
        context->CSSetShaderResources(0, static_cast<UINT>(std::size(sums)), sums);
        dispatch(context, integrateShader.Get(), output, groups(TileWidth), groups(TileHeight), 1);
        context->CSSetShaderResources(0, static_cast<UINT>(std::size(sums)), none);

        // The next fill reprojects from this frame's view.
        context->CopyResource(copies.lastSunScene.Get(), copies.sunScene.Get());
        visibilityWritten = next;
    }
    context->PSSetShaderResources(FogSlot, 1, readers.GetAddressOf());
    lastFillShadowed = shadowed;

    lastFill = GetTickCount64();
    logOnce(shadowed ? "Volumetric fog: the first fill, with the sun's shadow map." : "Volumetric fog: the first fill without a sun pass before it.");
    if (physical) logOnce("Volumetric fog: the first physical fill.");
}

// Every SkyLogInterval of the physical look: the sky parameters, for the log, read a frame after
// their copy so the CPU never waits for the GPU. On the immediate context, at present.
void logSkyParameters()
{
    std::lock_guard lock(resourceMutex);
    if (!skyParameterReadback || !immediate) return;

    if (skyReadbackPending)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(immediate->Map(skyParameterReadback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) return;

        float texels[SkyParameterTexels][4]{};
        std::memcpy(texels, mapped.pData, sizeof(texels));
        immediate->Unmap(skyParameterReadback.Get(), 0);
        skyReadbackPending = false;
        const auto number = [](float value) { return std::to_string(value); };
        log("Physical sky: sunlight above the air " + number(texels[0][0]) + " (the game's on the ground " + number(texels[5][0]) + ", its sky's "
            + number(texels[5][1]) + ", air transmittance " + number(texels[5][2]) + "), camera " + number(texels[0][3] * 1000) + " m, towards the sun "
            + number(texels[1][0]) + " " + number(texels[1][1]) + " " + number(texels[1][2]) + ", mean light " + number(texels[2][0]) + " "
            + number(texels[2][1]) + " " + number(texels[2][2]) + ", game fog " + number(texels[5][3]) + " /m, height fog " + number(texels[3][3])
            + " /m, overcast " + number(texels[6][3]) + ".");
        return;
    }

    const auto now = GetTickCount64();
    if (!skyReady || now - lastSkyLog < SkyLogInterval) return;

    ComPtr<ID3D11Resource> parameters;
    skyParameters.view->GetResource(parameters.GetAddressOf());
    immediate->CopyResource(skyParameterReadback.Get(), parameters.Get());
    skyReadbackPending = true;
    lastSkyLog = now;
}

// The physical sky in place of the game's sky draw, with its vertex shader and bindings. A sky with
// the sun's sprite also gives its constants to the next fill: the sun the game lights its sky with,
// and how overcast it draws it.
bool drawSky(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, bool sprite, ID3D11PixelShader* gameShader)
{
    std::array<ID3D11ShaderResourceView*, 2> gameViews{};
    bool copied{};
    {
        std::lock_guard lock(resourceMutex);
        if (!skyReady || !skyShaders[sprite ? 1 : 0]) return false;

        if (sprite)
        {
            ComPtr<ID3D11Buffer> material, object;
            context->PSGetConstantBuffers(MaterialSlot, 1, material.GetAddressOf());
            context->PSGetConstantBuffers(ObjectSlot, 1, object.GetAddressOf());
            copied = copyConstants(context, copies.skyMaterial.Get(), material.Get(), MaterialSize)
                && copyConstants(context, copies.skyObject.Get(), object.Get(), ObjectSize);
        }

        context->PSGetShaderResources(SkyParameterSlot, static_cast<UINT>(gameViews.size()), gameViews.data());
        ID3D11ShaderResourceView* const views[]{skyParameters.view.Get(), skyView.view.Get()};
        context->PSSetShader(skyShaders[sprite ? 1 : 0].Get(), nullptr, 0);
        context->PSSetShaderResources(SkyParameterSlot, static_cast<UINT>(std::size(views)), views);
    }

    performance::Timed timed(context, performance::Pass::Sky);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    timed.end();
    sky_capture::capture(context, call);
    context->PSSetShader(gameShader, nullptr, 0);
    context->PSSetShaderResources(SkyParameterSlot, static_cast<UINT>(gameViews.size()), gameViews.data());
    for (auto* view : gameViews)
        if (view) view->Release();

    // A sky without the sun (fog, sandstorms) leaves the next fill the ground's sun, not the last
    // sunny sky's, whose time of day may have passed.
    {
        std::lock_guard lock(captureMutex);
        skySunSeen = copied;
    }

    std::lock_guard lock(resourceMutex);
    logOnce("Volumetric fog: the physical sky draws in place of the game's.");

    return true;
}

}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* gameImmediate) noexcept
{
    const bool enable = latch.latest().running;

    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    immediate = gameImmediate;
    if (enable) ensurePipeline();
}

void detach() noexcept
{
    active = false;
    physicalLook = false;
    gameFog = nullptr;
    {
        std::lock_guard lock(resourceMutex);
        visibilityShader.Reset();
        mediaShader.Reset();
        integrateShader.Reset();
        transmittanceShader.Reset();
        multipleScatteringShader.Reset();
        parameterShader.Reset();
        skyViewShader.Reset();
        ambientShader.Reset();
        skyShaders = {};
        constantBuffer.Reset();
        linearClamp.Reset();
        shadowSampler.Reset();
        copies = {};
        media = {};
        fogOnly = {};
        currentVisibility = {};
        visibility = {};
        transmittance = {};
        multipleScattering = {};
        skyParameters = {};
        skyView = {};
        atmosphereTables = false;
        skyReady = false;
        skyParameterReadback.Reset();
        skyReadbackPending = false;
        lastFillShadowed = false;
        fogAccess.Reset();
        fogAccessOf = nullptr;
        device.Reset();
        immediate = nullptr;
        pipelineFailed = false;
    }

    {
        std::lock_guard lock(captureMutex);
        rainView.Reset();
        sunShadows.clear();
        fogPassSeen = false;
        skySunSeen = false;
    }
}

void configure(const Configuration& configuration) noexcept
{
    const bool running = configuration.effects.runs(Effect::VolumetricFog);
    latch.set({configuration.settings.volumetricFog, running});

    if (!running) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

Health health()
{
    if (!latch.latest().running) return {};
    if (pipelineFailed) return Health::unavailable("Volumetric fog is unavailable (see PhantomFX.log).");
    if (!gameFog) return Health::waiting("Volumetric fog takes a restart of the game.");
    if (GetTickCount64() - lastFill < 1000) return {};

    return Health::waiting("Volumetric fog is waiting for the game's 3D view.");
}

bool upgradeTexture(UINT& width, UINT& height, DXGI_FORMAT format, bool renderTarget) noexcept
{
    creatingFog = false;
    const bool fogFormat = format == DXGI_FORMAT_R16G16B16A16_TYPELESS || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!isSupportedGameBuild() || width != GameFogWidth || height != GameFogHeight || !fogFormat || !renderTarget) return false;

    width = FogWidth;
    height = FogHeight;
    creatingFog = true;

    return true;
}

void textureCreated(ID3D11Resource* texture) noexcept
{
    if (!std::exchange(creatingFog, false) || !texture) return;

    ComPtr<ID3D11Texture2D> fog;
    D3D11_TEXTURE2D_DESC desc{};
    if (SUCCEEDED(texture->QueryInterface(fog.GetAddressOf()))) fog->GetDesc(&desc);
    if (desc.Width != FogWidth || desc.Height != FogHeight || !(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) return;

    gameFog = texture;
    log("Volumetric fog: the game's fog texture is " + std::to_string(FogWidth) + "x" + std::to_string(FogHeight) + ".");
}

void resourceDestroyed(ID3D11Resource* resource) noexcept
{
    auto* expected = resource;
    if (!resource || !gameFog.compare_exchange_strong(expected, nullptr)) return;

    std::lock_guard lock(resourceMutex);
    if (fogAccessOf == resource)
    {
        fogAccess.Reset();
        fogAccessOf = nullptr;
    }
}

void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!gameFog.load(std::memory_order_relaxed) || !active.load(std::memory_order_relaxed) || !isFogOrFullScreen(call)) return;

    try
    {
        // The sun inside the shadow range binds the shadow map and its cascades; the resolves with TppTonemap
        // are the first to read the fog, after the shadow map is drawn.
        const auto* row = shader.row();
        if (!row) return;

        if (row->pass == GamePass::Sun && row->isShadowMapped) noteSun(context);
        else if (row->pass == GamePass::Resolve && row->hasTppTonemap) fill(context, physicalLook.load(std::memory_order_relaxed));
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("Volumetric fog: a pass kept the game's fog: ") + error.what());
    }
    catch (...)
    {
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!gameFog.load(std::memory_order_relaxed) || !isFogOrFullScreen(call)) return false;

    try
    {
        const auto* row = shader.row();
        if (!row) return false;

        const bool fog = active.load(std::memory_order_relaxed), physical = fog && physicalLook.load(std::memory_order_relaxed);
        if (row->pass == GamePass::FogPass) return call.count == FogPassVertices && drawFogPass(context, call, fog);
        if (row->pass == GamePass::Sky) return physical && drawSky(context, call, row->hasSprite, shader.object());
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("Volumetric fog: a pass kept the game's fog: ") + error.what());
    }
    catch (...)
    {
    }

    return false;
}

void present() noexcept
{
    const auto current = latch.present();
    active = current.running && !pipelineFailed;
    physicalLook = active && current.fog.look == AtmosphereLook::Physical;
    haze = current.fog.haze;

    if constexpr (DiagnosticLog)
    {
        if (physicalLook) logSkyParameters();
    }
}
}
