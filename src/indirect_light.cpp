#include "indirect_light.hpp"

#include "d3d11_helpers.hpp"
#include "global_illumination.hpp"
#include "indirect_light_shaders.hpp"
#include "local_lights.hpp"
#include "log.hpp"
#include "performance.hpp"
#include "shader_bytecode.hpp"

#include <Windows.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phantom_fx::indirect_light
{
namespace
{
using Microsoft::WRL::ComPtr;

// The game's pixel shaders GTVBAO and GT-VBGI work with (docs/DEVELOPMENT.md, "Ambient occlusion").
enum class Role : std::uint8_t
{
    SkyLight,      // Binds the G-buffer normals, right before the upscale in the same command list
    Upscale,       // Writes the indirect light into the diffuse light buffer
    Resolve,       // Multiplies all light by the game's occlusion
    GameOcclusion, // The game's SSAO, whose result only the resolve reads
    Sun,           // Binds the G-buffer's specular target, which holds the material
    SunShadowMap,  // The same, inside the shadow map's range
    SkySpecular,   // Indoors, where no sun shines, the sky's specular alone in the sun pass's place
};

struct GameShader
{
    Role role;
    const char* name;
    const char* checksum;
};

constexpr GameShader Shaders[]{
    {Role::SkyLight, "SSLighting2_SH_SkyLight_Encode", "ac49de83da6ebb80336de04c4ef7d022"},
    {Role::Upscale, "BilateralUpscale2x2", "1314ac1eb31bb06b4e972fdc2c24ccbf"},
    {Role::Resolve, "DR_VolFog_TppTonemap", "00a609370c2e8b47a645263f91f6d3e8"},
    {Role::Resolve, "DR_VolFog_TppTonemap_MD", "79c001e295254d8a749e41d1d7e49de7"},
    {Role::Resolve, "DR_TonemapRainFilter", "9a4abc815afc5f121572d695f07af5af"},
    {Role::Resolve, "DR_TonemapRainFilter_NoIR", "47f52852eaccaa83c25745ce790a212c"},
    {Role::Resolve, "DeferredRendering", "df87979884e90877ee0337c13e01664f"},
    {Role::Resolve, "DeferredRendering_VolFog", "322890810465ee05f4895bc69e80a187"},
    {Role::Resolve, "DeferredRenderingTone", "47256d38f45b59b6c46cf32bf29564f0"},
    {Role::Resolve, "DeferredRenderingTone_GVFog", "86a042757928c965fff1c016647c4dfa"},
    {Role::Resolve, "DeferredRenderingFilmic", "586e33bca62435478f23154da9e25187"},
    {Role::Resolve, "DeferredRenderingFilmic_VolFog", "504b446c3e795d75e6adcd6753c11d12"},
    {Role::GameOcclusion, "SSAOLineIntegrals", "1e716301e9c644c4d59c620e9b84f7de"},
    {Role::GameOcclusion, "AmbientObscuranceSSAO", "9f963f50539aafc2ee77b888c56d3a0f"},
    {Role::SunShadowMap, "SunLight_DL_CS_ES", "56ed4db892a7940f038146eeb0df0d30"},
    {Role::Sun, "SunLight_DL_ES", "92726e8d231c8ed85f0db90400ef2258"},
    {Role::SunShadowMap, "SunLight_DL_CS", "7e5c2fc99c9d84ddd32df4e789e7082a"}, // In rain
    {Role::Sun, "SunLight_DL", "853f56a63e5bc66e6ee301c85ebfeaa8"},    // In rain
    {Role::SkySpecular, "SunLight_ES", "367e8f6128befd48f2840ab3d6ed19b4"}, // Indoors
};

constexpr UINT NormalSlot = 0;     // The sky light's g_NormalTexture
constexpr UINT SpecularSlot = 1;   // The sun's g_SpecularTexture
constexpr UINT DepthSlot = 2;      // The upscale's inDepthBuffer
constexpr UINT SourceSlot = 0;     // The upscale's inSourceBuffer, where shaders::Apply reads the visibility
constexpr UINT SceneSlot = 2;      // cPSScene, in the upscale and the resolves
// Every resolve's inAlbedo, inLightDiffuse, inDepth and inOcclusion.
constexpr UINT AlbedoSlot = 8, DiffuseSlot = 9, ResolveDepthSlot = 11, OcclusionSlot = 13;
constexpr UINT SceneConstantsSize = 480;
constexpr int DenoisePasses = 2;
// GT-VBGI's march and denoiser run at the render size over this, rounded up (Scale in shaders::March).
constexpr UINT LightScale = 2;
// The march stops at this share of the render height, for surfaces close to the camera.
constexpr float MaxRadiusShare = 0.15f;
// shaders::PrefilterDepths writes the distances and four smaller copies (DistanceMipCount in
// shaders::March), each 8 x 8 group 16 x 16 pixels.
constexpr UINT DistanceMipCount = 5;
constexpr UINT PrefilterPixels = 16;

// The compute shaders' constant buffer (PHANTOM_FX_PARAMETERS in indirect_light_shaders.hpp).
struct Parameters
{
    std::uint32_t size[2];
    float radius, maxRadiusPixels, occlusionStrength, lightStrength, diffuseScale, padding;
};

using Target = ComputeTexture;

// A texture one of the game's pixel shaders draws into.
struct DrawTarget
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11RenderTargetView> target;
    UINT width{}, height{};
};

// A texture with its smaller copies: one view reads them all, one output view each writes them.
struct MipTarget
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    std::array<ComPtr<ID3D11UnorderedAccessView>, DistanceMipCount> access;
    UINT width{}, height{};
};

// When an effect last ran, and otherwise what it waits for, for the overlay.
struct Progress
{
    const char* name;
    std::uint64_t lastRun{}; // GetTickCount64
    std::string waitingFor;
};

// What the overlay asked for, and what the frame being recorded uses.
std::mutex optionsMutex;
Options wanted, current;
std::atomic<bool> active{}, pipelineFailed{};

// Identified game shaders, by the object the game draws with. Holding them keeps their addresses
// from being reused.
std::shared_mutex shaderMutex;
std::unordered_map<IUnknown*, Role> roles;
std::vector<ComPtr<IUnknown>> shaderOwners;

// The normals each context's sky light pass bound, for the upscale it records next. The resolve is
// in another command list; it takes the G-buffer normals any sky light pass bound last. The sun pass
// comes after the upscale, so the march takes the specular target from an earlier frame's: the
// same texture while the render size stays.
std::mutex gBufferMutex;
std::unordered_map<ID3D11DeviceContext*, ComPtr<ID3D11ShaderResourceView>> normals;
ComPtr<ID3D11ShaderResourceView> lastNormals, lastSpecular;

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> prefilterShader, occlusionShader, denoiseShader, finalDenoiseShader;
ComPtr<ID3D11ComputeShader> lightShader, lightDenoiseShader, finalLightDenoiseShader, lightUpsampleShader, addLightShader;
ComPtr<ID3D11PixelShader> applyShader, showOcclusionShader, showLightShader;
ComPtr<ID3D11BlendState> multiplyBlend;
ComPtr<ID3D11Buffer> parameterBuffer;
ComPtr<ID3D11ShaderResourceView> whiteView; // 1x1, the resolve's occlusion while GTVBAO is on
// The denoise passes alternate between the two; the last one writes [Result]. GT-VBGI's are at
// its march's size.
std::array<Target, 2> visibility, gathered;
constexpr std::size_t Result = DenoisePasses % 2;
Target occlusionEdges, lightEdges; // Depth edges for each denoiser, written by its march
Target diffuseWithLight;           // The game's diffuse light with GT-VBGI's or the global illumination's added, read by the resolve in its place
// While the global illumination runs, the game's probe light, which the upscale also draws aside:
// the resolve takes it out of the diffuse light. The game's light target keeps it, as the game's
// exposure (Tonemap_Lum_Lacc) measures that target: without the probe light there, the exposure
// ran away in the shade (a white screen).
DrawTarget gameProbeLight;
MipTarget distances;               // shaders::PrefilterDepths' output, written before each march
Progress occlusionProgress{"GTVBAO"}, lightProgress{"GT-VBGI"}, globalProgress{"Global illumination"};
// Compiles the shaders and makes the fixed resources once; call with resourceMutex held. Returns
// false when GTVBAO and GT-VBGI are unavailable.
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
        lightShader = computeShader(device.Get(), shaders::March, "gtvbgi", lightPass);
        lightDenoiseShader = computeShader(device.Get(), shaders::Denoise, "gtvbgi-denoise", lightPass);
        finalLightDenoiseShader = computeShader(device.Get(), shaders::Denoise, "gtvbgi-denoise-final", lastLightPass);
        lightUpsampleShader = computeShader(device.Get(), shaders::Upsample, "gtvbgi-upsample");
        addLightShader = computeShader(device.Get(), shaders::AddLight, "gi-add");
        applyShader = pixelShader(device.Get(), shaders::Apply, "gtvbao-apply");
        showOcclusionShader = pixelShader(device.Get(), shaders::Show, "gtvbao-show", occlusionPass);
        showLightShader = pixelShader(device.Get(), shaders::Show, "gtvbgi-show", lightPass);

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

        parameterBuffer = makeConstantBuffer(device.Get(), sizeof(Parameters), "Creating the march constants");

        D3D11_TEXTURE2D_DESC white{};
        white.Width = white.Height = white.MipLevels = white.ArraySize = 1;
        white.Format = DXGI_FORMAT_R8_UNORM;
        white.SampleDesc.Count = 1;
        white.Usage = D3D11_USAGE_IMMUTABLE;
        white.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        constexpr std::uint8_t unoccluded = 255;
        const D3D11_SUBRESOURCE_DATA data{&unoccluded, 1, 1};
        ComPtr<ID3D11Texture2D> whiteTexture;
        check(device->CreateTexture2D(&white, &data, whiteTexture.GetAddressOf()), "Creating the white occlusion");
        check(device->CreateShaderResourceView(whiteTexture.Get(), nullptr, whiteView.ReleaseAndGetAddressOf()), "Creating the white occlusion");

        // Last, so it marks a complete pipeline.
        occlusionShader = computeShader(device.Get(), shaders::March, "gtvbao", occlusionPass);
        log("GTVBAO and GT-VBGI ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("GTVBAO and GT-VBGI unavailable: ") + error.what());

        return false;
    }
}

// Makes `target` at the size, unless it has it; call with resourceMutex held. Command lists that
// still use the previous texture hold their own references. Returns true when it made one.
bool ensureTarget(Target& target, UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    if (target.texture && target.width == width && target.height == height) return false;

    target = makeComputeTexture(device.Get(), width, height, format, what);

    return true;
}

// Call with resourceMutex held.
void ensureDrawTarget(DrawTarget& target, UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    if (target.texture && target.width == width && target.height == height) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    DrawTarget made{.width = width, .height = height};
    check(device->CreateTexture2D(&desc, nullptr, made.texture.GetAddressOf()), what);
    check(device->CreateShaderResourceView(made.texture.Get(), nullptr, made.view.GetAddressOf()), what);
    check(device->CreateRenderTargetView(made.texture.Get(), nullptr, made.target.GetAddressOf()), what);
    target = std::move(made);
}

// The prefiltered distances at the render size; call with resourceMutex held.
void ensureDistances(UINT width, UINT height)
{
    if (distances.texture && distances.width == width && distances.height == height) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = DistanceMipCount;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    MipTarget made{.width = width, .height = height};
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

    distances = std::move(made);
}

// GTVBAO's working textures at the render size; call with resourceMutex held.
void ensureOcclusionTargets(UINT width, UINT height)
{
    ensureDistances(width, height);
    bool made = ensureTarget(occlusionEdges, width, height, DXGI_FORMAT_R8_UNORM, "Creating the GTVBAO depth edges");
    for (auto& target : visibility) made |= ensureTarget(target, width, height, DXGI_FORMAT_R8_UNORM, "Creating the GTVBAO visibility");
    if (made) log("GTVBAO at " + std::to_string(width) + "x" + std::to_string(height) + ".");
}

// GT-VBGI's working textures: the march's at its size, the diffuse light at the render size; call
// with resourceMutex held.
void ensureLightTargets(UINT width, UINT height)
{
    ensureDistances(width, height);
    const UINT marchWidth = (width + LightScale - 1) / LightScale, marchHeight = (height + LightScale - 1) / LightScale;
    bool made = ensureTarget(lightEdges, marchWidth, marchHeight, DXGI_FORMAT_R8_UNORM, "Creating the GT-VBGI depth edges");
    for (auto& target : gathered)
        made |= ensureTarget(target, marchWidth, marchHeight, DXGI_FORMAT_R11G11B10_FLOAT, "Creating the GT-VBGI light");
    // The game's diffuse light's format.
    made |= ensureTarget(diffuseWithLight, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the GT-VBGI diffuse light");
    if (made)
        log("GT-VBGI at " + std::to_string(marchWidth) + "x" + std::to_string(marchHeight) + " for " + std::to_string(width) + "x"
            + std::to_string(height) + ".");
}

std::optional<Role> roleOf(IUnknown* shader)
{
    if (!shader) return std::nullopt;

    std::shared_lock lock(shaderMutex);
    const auto found = roles.find(shader);

    return found != roles.end() ? std::optional(found->second) : std::nullopt;
}

// The game's compute slots the passes bind, saved around them.
constexpr ComputeSlots PassSlots{.buffers = 5, .views = 5, .accesses = 5};

void writeParameters(ID3D11DeviceContext* context, UINT width, UINT height, float radius, const Options& options)
{
    const bool global = options.globalIllumination;
    const Parameters parameters{{width, height}, radius, MaxRadiusShare * static_cast<float>(height), options.occlusionStrength,
        global ? options.globalIlluminationStrength : options.lightStrength,
        (global ? options.showGlobalIllumination : options.showLight) ? 0.0f : 1.0f, 0};
    check(write(context, parameterBuffer.Get(), parameters), "Writing the march constants");
}

// Records shaders::PrefilterDepths into `distances`, with the march's constants bound, and unbinds
// them for the march to read.
void prefilterDepths(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* specular)
{
    ID3D11ShaderResourceView* inputs[]{depth, specular};
    ID3D11UnorderedAccessView* outputs[DistanceMipCount]{};
    for (UINT mip = 0; mip < DistanceMipCount; ++mip) outputs[mip] = distances.access[mip].Get();
    context->CSSetShaderResources(0, 2, inputs);
    context->CSSetUnorderedAccessViews(0, DistanceMipCount, outputs, nullptr);
    context->CSSetShader(prefilterShader.Get(), nullptr, 0);
    context->Dispatch((distances.width + PrefilterPixels - 1) / PrefilterPixels, (distances.height + PrefilterPixels - 1) / PrefilterPixels, 1);

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

// Records GTVBAO's march and denoise passes on `context`; call with resourceMutex held.
void computeOcclusion(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* normalView,
    ID3D11ShaderResourceView* specular, ID3D11Buffer* scene, const Options& options)
{
    const auto width = visibility[0].width, height = visibility[0].height;
    writeParameters(context, width, height, options.occlusionRadius, options);

    SavedComputeState saved(context, PassSlots);
    ID3D11Buffer* buffers[]{parameterBuffer.Get(), scene};
    context->CSSetConstantBuffers(0, 2, buffers);
    performance::Timed timed(context, performance::Pass::OcclusionPrefilter);
    prefilterDepths(context, depth, specular);

    timed.next(performance::Pass::OcclusionMarch);
    // t2 and t3 are GT-VBGI's.
    ID3D11ShaderResourceView* inputs[]{depth, normalView, nullptr, nullptr, distances.view.Get()};
    ID3D11UnorderedAccessView* outputs[]{visibility[0].access.Get(), occlusionEdges.access.Get()};
    context->CSSetShaderResources(0, 5, inputs);
    context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
    context->CSSetShader(occlusionShader.Get(), nullptr, 0);
    context->Dispatch(groups(width), groups(height), 1);
    timed.next(performance::Pass::OcclusionDenoise);
    denoise(context, visibility, occlusionEdges, denoiseShader.Get(), finalDenoiseShader.Get());
}

// Records GT-VBGI's march, denoise and upsample passes on `context`; call with resourceMutex held.
// The upsample writes diffuseWithLight.
void computeLight(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* normalView,
    ID3D11ShaderResourceView* diffuse, ID3D11ShaderResourceView* albedo, ID3D11ShaderResourceView* specular, ID3D11Buffer* scene,
    const Options& options)
{
    const auto width = diffuseWithLight.width, height = diffuseWithLight.height;
    writeParameters(context, width, height, options.lightRadius, options);

    SavedComputeState saved(context, PassSlots);
    ID3D11Buffer* buffers[]{parameterBuffer.Get(), scene};
    context->CSSetConstantBuffers(0, 2, buffers);
    performance::Timed timed(context, performance::Pass::LightPrefilter);
    prefilterDepths(context, depth, specular);

    timed.next(performance::Pass::LightMarch);
    ID3D11ShaderResourceView* inputs[]{depth, normalView, diffuse, albedo, distances.view.Get()};
    ID3D11UnorderedAccessView* outputs[]{gathered[0].access.Get(), lightEdges.access.Get()};
    context->CSSetShaderResources(0, 5, inputs);
    context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
    context->CSSetShader(lightShader.Get(), nullptr, 0);
    context->Dispatch(groups(gathered[0].width), groups(gathered[0].height), 1);
    timed.next(performance::Pass::LightDenoise);
    denoise(context, gathered, lightEdges, lightDenoiseShader.Get(), finalLightDenoiseShader.Get());

    timed.next(performance::Pass::LightUpsample);
    // The denoiser left gathered[Result] bound as an output, which would refuse it as an input.
    ID3D11UnorderedAccessView* none[2]{};
    ID3D11ShaderResourceView* upsampleInputs[]{gathered[Result].view.Get(), depth, diffuse};
    auto* output = diffuseWithLight.access.Get();
    context->CSSetUnorderedAccessViews(0, 2, none, nullptr);
    context->CSSetShaderResources(0, 3, upsampleInputs);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(lightUpsampleShader.Get(), nullptr, 0);
    context->Dispatch(groups(width), groups(height), 1);
}

// Draws the upscale once more with shaders::Apply and the multiplying blend; call with resourceMutex held.
void applyToIndirectLight(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    ComPtr<ID3D11PixelShader> gameShader;
    ComPtr<ID3D11ShaderResourceView> gameSource;
    ComPtr<ID3D11BlendState> gameBlend;
    float blendFactor[4]{};
    UINT sampleMask{};
    context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->PSGetShaderResources(SourceSlot, 1, gameSource.GetAddressOf());
    context->OMGetBlendState(gameBlend.GetAddressOf(), blendFactor, &sampleMask);

    context->PSSetShader(applyShader.Get(), nullptr, 0);
    context->PSSetShaderResources(SourceSlot, 1, visibility[Result].view.GetAddressOf());
    context->OMSetBlendState(multiplyBlend.Get(), nullptr, 0xFFFFFFFF);
    performance::Timed timed(context, performance::Pass::OcclusionApply);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    timed.end();

    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetShaderResources(SourceSlot, 1, gameSource.GetAddressOf());
    context->OMSetBlendState(gameBlend.Get(), blendFactor, sampleMask);
}

// Leaves the game's pass alone this frame; call with resourceMutex held.
bool wait(Progress& progress, std::string reason)
{
    logOnce(std::string(progress.name) + " waiting: " + reason + ".");
    progress.waitingFor = std::move(reason);
    return false;
}

void ran(Progress& progress)
{
    progress.lastRun = GetTickCount64();
    progress.waitingFor.clear();
}

void noteNormals(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11ShaderResourceView> view;
    context->PSGetShaderResources(NormalSlot, 1, view.GetAddressOf());
    std::lock_guard lock(gBufferMutex);
    lastNormals = view;
    normals[context] = std::move(view);
}

void noteSpecular(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11ShaderResourceView> view;
    context->PSGetShaderResources(SpecularSlot, 1, view.GetAddressOf());
    std::lock_guard lock(gBufferMutex);
    lastSpecular = std::move(view);
}

ComPtr<ID3D11ShaderResourceView> takeNormals(ID3D11DeviceContext* context)
{
    std::lock_guard lock(gBufferMutex);
    const auto found = normals.find(context);
    if (found == normals.end()) return nullptr;

    auto view = std::move(found->second);
    normals.erase(found);

    return view;
}

ComPtr<ID3D11ShaderResourceView> latestNormals()
{
    std::lock_guard lock(gBufferMutex);
    return lastNormals;
}

// The specular target a sun pass bound last, if it has the render size; otherwise none, and the
// march treats nothing as foliage.
ComPtr<ID3D11ShaderResourceView> latestSpecular(std::pair<UINT, UINT> size)
{
    ComPtr<ID3D11ShaderResourceView> view;
    {
        std::lock_guard lock(gBufferMutex);
        view = lastSpecular;
    }

    return sizeOf(view.Get()) == size ? view : nullptr;
}

}

ComPtr<ID3D11ShaderResourceView> specularTarget(UINT width, UINT height) noexcept
{
    try
    {
        return latestSpecular({width, height});
    }
    catch (...)
    {
        return nullptr;
    }
}

namespace
{
// The game's upscale, then GTVBAO's occlusion multiplied into what it wrote. False leaves the draw to the game.
bool drawUpscale(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const Options& options)
{
    const auto normalView = takeNormals(context);
    ComPtr<ID3D11ShaderResourceView> depth;
    ComPtr<ID3D11Buffer> scene;
    ComPtr<ID3D11RenderTargetView> light;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    context->PSGetConstantBuffers(SceneSlot, 1, scene.GetAddressOf());
    context->OMGetRenderTargets(1, light.GetAddressOf(), nullptr);
    D3D11_BUFFER_DESC sceneDesc{};
    if (scene) scene->GetDesc(&sceneDesc);

    const auto size = sizeOf(depth.Get());
    const auto specular = latestSpecular(size);
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;
    if (!normalView) return wait(occlusionProgress, "the sky light pass bound no normals before the upscale");
    if (!size.first || sizeOf(normalView.Get()) != size || sizeOf(light.Get()) != size)
        return wait(occlusionProgress, "the lighting textures differ in size");
    if (sceneDesc.ByteWidth != SceneConstantsSize) return wait(occlusionProgress, "the upscale has no scene constants");

    ensureOcclusionTargets(size.first, size.second);
    computeOcclusion(context, depth.Get(), normalView.Get(), specular.Get(), scene.Get(), options);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    applyToIndirectLight(context, call);
    ran(occlusionProgress);

    return true;
}

// GT-VBGI's passes, from what the resolve binds; call with resourceMutex held. False leaves the
// resolve the game's diffuse light this frame.
bool addLight(ID3D11DeviceContext* context, ID3D11ShaderResourceView* normalView, const Options& options)
{
    ComPtr<ID3D11ShaderResourceView> depth, diffuse, albedo;
    ComPtr<ID3D11Buffer> scene;
    context->PSGetShaderResources(ResolveDepthSlot, 1, depth.GetAddressOf());
    context->PSGetShaderResources(DiffuseSlot, 1, diffuse.GetAddressOf());
    context->PSGetShaderResources(AlbedoSlot, 1, albedo.GetAddressOf());
    context->PSGetConstantBuffers(SceneSlot, 1, scene.GetAddressOf());
    D3D11_BUFFER_DESC sceneDesc{};
    if (scene) scene->GetDesc(&sceneDesc);

    const auto size = sizeOf(depth.Get());
    if (!normalView) return wait(lightProgress, "no sky light pass has bound the normals yet");
    if (!size.first || sizeOf(normalView) != size || sizeOf(diffuse.Get()) != size || sizeOf(albedo.Get()) != size)
        return wait(lightProgress, "the lighting textures differ in size");
    if (sceneDesc.ByteWidth != SceneConstantsSize) return wait(lightProgress, "the resolve has no scene constants");

    ensureLightTargets(size.first, size.second);
    computeLight(context, depth.Get(), normalView, diffuse.Get(), albedo.Get(), latestSpecular(size).Get(), scene.Get(), options);
    ran(lightProgress);

    return true;
}

// The resolve, with the diffuse light GT-VBGI added to while it runs, and white in place of the
// game's occlusion while GTVBAO runs; or, for the diagnostics, either effect alone. False leaves
// the draw to the game.
bool drawResolve(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const Options& options)
{
    const auto normalView = options.light ? latestNormals() : nullptr;
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    const bool lit = options.light && addLight(context, normalView.Get(), options);
    if (!lit && !options.occlusion) return false;

    ComPtr<ID3D11PixelShader> gameShader;
    ComPtr<ID3D11ShaderResourceView> gameDiffuse, gameOcclusion;
    context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->PSGetShaderResources(DiffuseSlot, 1, gameDiffuse.GetAddressOf());
    context->PSGetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());

    if (lit) context->PSSetShaderResources(DiffuseSlot, 1, diffuseWithLight.view.GetAddressOf());
    if (options.occlusion) context->PSSetShaderResources(OcclusionSlot, 1, whiteView.GetAddressOf());
    if (lit && options.showLight) context->PSSetShader(showLightShader.Get(), nullptr, 0);
    else if (options.occlusion && options.showOcclusion && visibility[Result].view)
    {
        context->PSSetShader(showOcclusionShader.Get(), nullptr, 0);
        context->PSSetShaderResources(OcclusionSlot, 1, visibility[Result].view.GetAddressOf());
    }

    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetShaderResources(DiffuseSlot, 1, gameDiffuse.GetAddressOf());
    context->PSSetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());

    return true;
}

// What the ray-traced scene needs from the game's passes (global_illumination.hpp): the normals, the
// kick at the upscale, the wait at the resolve, and the sun; the first of the sun pass, the sky's specular
// pass and the resolve draws the point and spot lights too.
void noteForRayTracing(ID3D11DeviceContext* context, Role role)
{
    ComPtr<ID3D11ShaderResourceView> view;
    switch (role)
    {
    case Role::SkyLight:
        context->PSGetShaderResources(NormalSlot, 1, view.GetAddressOf());
        global_illumination::noteNormals(context, view.Get());
        return;
    case Role::Upscale:
        context->PSGetShaderResources(DepthSlot, 1, view.GetAddressOf());
        global_illumination::noteUpscale(context, view.Get());
        return;
    case Role::Resolve:
        context->PSGetShaderResources(AlbedoSlot, 1, view.GetAddressOf());
        global_illumination::noteResolve(context, view.Get());
        local_lights::noteDrawPoint(context);
        return;
    case Role::Sun:
        global_illumination::noteSun(context, false);
        local_lights::noteDrawPoint(context);
        return;
    case Role::SunShadowMap:
        global_illumination::noteSun(context, true);
        local_lights::noteDrawPoint(context);
        return;
    case Role::SkySpecular:
        global_illumination::noteSkySpecular(context);
        local_lights::noteDrawPoint(context);
        return;
    case Role::GameOcclusion: return;
    }
}

// The upscale while the global illumination runs: drawn aside into gameProbeLight first, then by
// the game as always. False leaves the draw to the game, which it always does.
bool drawUpscaleForGlobal(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    if (!global_illumination::running()) return false;

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    ID3D11RenderTargetView* gameTargets[2]{};
    ComPtr<ID3D11DepthStencilView> gameDepth;
    context->OMGetRenderTargets(2, gameTargets, gameDepth.GetAddressOf());
    std::array<ComPtr<ID3D11RenderTargetView>, 2> heldTargets;
    for (int i = 0; i < 2; ++i) heldTargets[i].Attach(gameTargets[i]);

    // The game draws it with blending off, into the light targets.
    const auto size = sizeOf(gameTargets[0]);
    ensureDrawTarget(gameProbeLight, size.first, size.second, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the game's probe light");
    const performance::Timed timed(context, performance::Pass::GiProbeLight);
    context->OMSetRenderTargets(1, gameProbeLight.target.GetAddressOf(), gameDepth.Get());
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->OMSetRenderTargets(2, gameTargets, gameDepth.Get());

    return false;
}

// The resolve while the global illumination runs: the diffuse light with the traced light in place
// of the game's probe light, white in place of the game's occlusion; or, for the diagnostics, the
// global illumination's output alone. False leaves the draw to the game.
bool drawResolveForGlobal(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const Options& options)
{
    if (!global_illumination::running()) return false;

    ComPtr<ID3D11ShaderResourceView> depth, diffuse, gameOcclusion;
    ComPtr<ID3D11PixelShader> gameShader;
    context->PSGetShaderResources(ResolveDepthSlot, 1, depth.GetAddressOf());
    context->PSGetShaderResources(DiffuseSlot, 1, diffuse.GetAddressOf());

    const auto size = sizeOf(depth.Get());
    auto* light = global_illumination::light(size.first, size.second);
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;
    if (!light || !size.first || sizeOf(diffuse.Get()) != size) return wait(globalProgress, "the lighting textures differ in size");

    ensureTarget(diffuseWithLight, size.first, size.second, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the lit diffuse light");
    // Before the first upscale at this size, nothing is taken out.
    const bool hasProbeLight = gameProbeLight.width == size.first && gameProbeLight.height == size.second;
    writeParameters(context, size.first, size.second, 0, options);
    {
        const performance::Timed timed(context, performance::Pass::GiCombine);
        SavedComputeState saved(context, PassSlots);
        context->CSSetConstantBuffers(0, 1, parameterBuffer.GetAddressOf());
        ID3D11ShaderResourceView* inputs[]{diffuse.Get(), light, hasProbeLight ? gameProbeLight.view.Get() : nullptr};
        auto* output = diffuseWithLight.access.Get();
        context->CSSetShaderResources(0, 3, inputs);
        context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        context->CSSetShader(addLightShader.Get(), nullptr, 0);
        context->Dispatch(groups(size.first), groups(size.second), 1);
    }

    context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->PSGetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());
    if (options.showGlobalIllumination)
    {
        context->PSSetShader(showLightShader.Get(), nullptr, 0);
        context->PSSetShaderResources(DiffuseSlot, 1, &light);
    }
    else context->PSSetShaderResources(DiffuseSlot, 1, diffuseWithLight.view.GetAddressOf());

    context->PSSetShaderResources(OcclusionSlot, 1, whiteView.GetAddressOf());
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetShaderResources(DiffuseSlot, 1, diffuse.GetAddressOf());
    context->PSSetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());
    ran(globalProgress);

    return true;
}

// Call with resourceMutex held.
std::string problemOf(const Progress& progress)
{
    const std::string name = progress.name;
    if (pipelineFailed) return name + " is unavailable (see PhantomFX.log).";
    if (progress.lastRun && GetTickCount64() - progress.lastRun < 1000) return {};

    return progress.waitingFor.empty() ? name + " is waiting for the game's 3D view." : name + " is waiting: " + progress.waitingFor + ".";
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    bool enable{};
    {
        std::lock_guard lock(optionsMutex);
        enable = wanted.occlusion || wanted.light || wanted.globalIllumination;
    }

    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (enable) ensurePipeline();
}

void detach() noexcept
{
    active = false;
    {
        std::lock_guard lock(resourceMutex);
        prefilterShader.Reset();
        occlusionShader.Reset();
        denoiseShader.Reset();
        finalDenoiseShader.Reset();
        lightShader.Reset();
        lightDenoiseShader.Reset();
        finalLightDenoiseShader.Reset();
        lightUpsampleShader.Reset();
        addLightShader.Reset();
        applyShader.Reset();
        showOcclusionShader.Reset();
        showLightShader.Reset();
        multiplyBlend.Reset();
        parameterBuffer.Reset();
        whiteView.Reset();
        visibility = {};
        gathered = {};
        occlusionEdges = {};
        lightEdges = {};
        distances = {};
        diffuseWithLight = {};
        gameProbeLight = {};
        device.Reset();
        pipelineFailed = false;
    }

    {
        std::lock_guard lock(gBufferMutex);
        normals.clear();
        lastNormals.Reset();
        lastSpecular.Reset();
    }

    std::unique_lock lock(shaderMutex);
    roles.clear();
    shaderOwners.clear();
}

void configure(const Options& options) noexcept
{
    {
        std::lock_guard lock(optionsMutex);
        wanted = options;
    }

    if (!options.occlusion && !options.light && !options.globalIllumination) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

std::string occlusionProblem()
{
    {
        std::lock_guard lock(optionsMutex);
        if (!wanted.occlusion) return {};
    }

    std::lock_guard lock(resourceMutex);
    return problemOf(occlusionProgress);
}

std::string lightProblem()
{
    {
        std::lock_guard lock(optionsMutex);
        if (!wanted.light) return {};
    }

    std::lock_guard lock(resourceMutex);
    return problemOf(lightProgress);
}

void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    const auto text = toHex(*checksum);
    for (const auto& known : Shaders)
    {
        if (text != known.checksum) continue;

        try
        {
            std::unique_lock lock(shaderMutex);
            roles[shader] = known.role;
            shaderOwners.emplace_back(shader);
        }
        catch (...)
        {
            return;
        }

        log(std::string("Identified ") + known.name + ".");
        return;
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (!active.load(std::memory_order_relaxed) || call.indexed || call.count != 3 || call.instances != 1) return false;

    try
    {
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        const auto role = roleOf(shader.Get());
        if (!role) return false;

        Options options;
        {
            std::lock_guard lock(optionsMutex);
            options = current;
        }

        if (options.rayTracing) noteForRayTracing(context, *role);
        if (options.globalIllumination)
            switch (*role)
            {
            case Role::Upscale: return drawUpscaleForGlobal(context, call);
            case Role::Resolve: return drawResolveForGlobal(context, call, options);
            case Role::GameOcclusion: return global_illumination::running();
            case Role::Sun:
            case Role::SunShadowMap: noteSpecular(context); return false;
            case Role::SkyLight:
            case Role::SkySpecular: return false;
            }

        switch (*role)
        {
        case Role::SkyLight: noteNormals(context); return false;
        case Role::Upscale: return options.occlusion && drawUpscale(context, call, options);
        case Role::Resolve: return drawResolve(context, call, options);
        case Role::GameOcclusion: return options.occlusion;
        case Role::Sun:
        case Role::SunShadowMap: noteSpecular(context); return false;
        case Role::SkySpecular: return false;
        }
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("GTVBAO or GT-VBGI: a pass kept the game's drawing: ") + error.what());
    }
    catch (...)
    {
    }

    return false;
}

void present() noexcept
{
    std::lock_guard lock(optionsMutex);
    current = wanted;
    active = (current.occlusion || current.light || current.globalIllumination || current.rayTracing) && !pipelineFailed;
}
}
