// The ray-traced scene's frame on Direct3D 11; ray_tracing.hpp describes the route.
#include "passes/ray_tracing/ray_tracing.hpp"

#include "camera.hpp"
#include "clock.hpp"
#include "command_list_notes.hpp"
#include "constant_capture.hpp"
#include "d3d11_helpers.hpp"
#include "features/global_illumination/gi_lighting.hpp"
#include "features/forward_shadows/forward_shadows.hpp"
#include "features/path_tracing/forward_surfaces.hpp"
#include "features/path_tracing/path_tracing.hpp"
#include "features/path_tracing/sky_capture.hpp"
#include "features/ray_traced_lights/lamp_shadow_rays.hpp"
#include "features/ray_traced_lights/local_lights.hpp"
#include "features/sea/sea.hpp"
#include "frame_latch.hpp"
#include "game_constants.hpp"
#include "log.hpp"
#include "passes/indirect_light_pass.hpp"
#include "passes/ray_tracing/engine_scene.hpp"
#include "passes/ray_tracing/gi_inputs.hpp"
#include "passes/ray_tracing/gi_renderer.hpp"
#include "passes/ray_tracing/gi_textures.hpp"
#include "passes/ray_tracing/kick_traces.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"
#include "passes/ray_tracing/trace.hpp"
#include "performance.hpp"

#include <Windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace phantom_fx::ray_tracing
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_constants::SceneConstants;

struct Options
{
    kick_traces::Asked asked;
    // With the shadows: the game's shadow map adds its shadow near the camera, so the rays leave it cut-out
    // geometry without its texture there. Without, they take that geometry as letting half the sun through.
    bool withShadowMap{};
    int lightRays{4};   // The point and spot lights' shadow rays a pixel
    float sunSize{0.53f}; // The sun's angular diameter for the shadows and the light's hits, in degrees
    int lightPixelRays{2}; // The light's rays a light pixel (a 2 x 2 of render pixels): 1, 2 or 4
    bool debugShadows{}; // Diagnostics: the shadow debug view and probe (shadowDebug, shadowProbe), with the shadows
    bool shadowStarts{}; // Diagnostics: the debug view shows where the shadow rays leave each pixel
    bool twoSidedShadows{}; // Diagnostics: every surface blocks rays towards lights from both sides, as until 2026-10-26
    gi_renderer::LightDebugView lightDebugView{}; // Diagnostics: over the image (lightDebug)

    bool logOften{};   // Diagnostics: the light's statistics logged every few frames (as each readback returns), not every ten seconds
    float emission{1}; // The menu's strength of self-lit meshes' light
};

// MotionBlurCameraVelocity reads the game's velocity for its moving meshes here, right after the
// G-buffer (docs/DEVELOPMENT.md, "Velocity").
constexpr UINT ObjectVelocitySlot = 3;

// Packs what the kernels read from the G-buffer: the depth, the normals in world space (0 where
// the sky is; alpha 1/3 on foliage, the specular target's material 43, else 1) and the albedo, linear.
constexpr char PackSource[] = R"(
Texture2D<float> g_Depth : register(t0);
Texture2D<float4> g_Normal : register(t1);
Texture2D<float4> g_Albedo : register(t2);
Texture2D<float4> g_Specular : register(t3);
RWTexture2D<float> g_DepthOut : register(u0);
RWTexture2D<float4> g_NormalOut : register(u1);
RWTexture2D<float4> g_AlbedoOut : register(u2);

cbuffer Parameters : register(b0)
{
    float4 g_View[3]; // World to view, rows
    uint2 g_Size;
    uint g_HasAlbedo;
    uint g_HasSpecular;
};

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= g_Size)) return;

    float depth = g_Depth.Load(int3(id, 0));
    g_DepthOut[id] = depth;

    // DecodeViewSpaceNormal: z = 2 b^2 - 1, xy the direction of 2 rg - 1 at length sqrt(1 - z^2).
    float3 encoded = g_Normal.Load(int3(id, 0)).xyz;
    float2 xy = encoded.xy * 2 - 1;
    float z = encoded.z * encoded.z * 2 - 1;
    float rest = 1 - z * z;
    float3 view = float3(rsqrt(dot(xy, xy) * rest + 1e-7) * (rest * xy), z);
    float3 world = normalize(view.x * g_View[0].xyz + view.y * g_View[1].xyz + view.z * g_View[2].xyz);
    bool foliage = g_HasSpecular && round(g_Specular.Load(int3(id, 0)).z * 255) == 43;
    g_NormalOut[id] = depth > 0 ? float4(0.5 * world + 0.5, foliage ? 1.0 / 3 : 1) : 0;

    g_AlbedoOut[id] = float4(g_HasAlbedo ? g_Albedo.Load(int3(id, 0)).rgb : 0.25, 1);
}
)";

struct PackParameters
{
    float view[3][4];
    std::uint32_t width, height, hasAlbedo, hasSpecular;
};

// DLSS Ray Reconstruction's guides from the G-buffer, as its integration guide asks for them: the
// world-space normal with the roughness in w, the diffuse albedo, and the specular albedo, the share of
// light the surface reflects towards the eye. That share is the game's for its sky reflection
// (SKY_REFLECTION in sun_light_pass_shaders.hpp): its specular colour (the material table's, times the
// specular target's intensity) through its Fresnel table at the eye's angle, from at least the sun pass's
// least F0 (cPSLight c2.z), times half the smoothness. The game's materials are tuned to it: with a
// physical one (NVIDIA's EnvBRDFApprox2), Snake's red arm, an 11% white specular of roughness 0.59,
// reflected 3 to 4 times as much and the sky's light turned it grey. The sea (sea.hpp) is not one of the
// game's materials: it reflects as water does, all of its light at grazing angles. Where the sky is: no
// normal and roughness, albedo 0.5, no specular albedo. The specular albedo's alpha is 0 on the sea, which the game's
// reflection blend leaves to its own reflectance (reflections.hlsl), and on the sky.
constexpr char GuideSource[] = R"(
Texture2D<float> g_Depth : register(t0);
Texture2D<float4> g_Normal : register(t1);
Texture2D<float4> g_Albedo : register(t2);    // Through an sRGB view: linear
Texture2D<float4> g_Specular : register(t3);  // Roughness, intensity, material, translucency
Texture2D<float4> g_Fresnel : register(t4);   // g_lut1: u the cosine, v F0
Texture2D<float4> g_Materials : register(t5); // g_MaterialTexture: u the material, row 0.25 its colour, 0.75 its F0
Texture2D<float> g_SeaDepth : register(t6);   // The sea's depth where it drew (sea::surfaceDepth)
Texture3D<float4> g_SpecularLobe : register(t7); // g_lut2: x the halfway vector's angle, y the roughness, z the anisotropy
RWTexture2D<float4> g_NormalRoughness : register(u0);
RWTexture2D<float4> g_DiffuseAlbedo : register(u1);
RWTexture2D<float4> g_SpecularAlbedo : register(u2);
RWTexture2D<float4> g_Material : register(u3); // Path tracing's: rgb the specular colour; a the translucency
RWTexture2D<float4> g_Lobe : register(u4);     // x the light through the leaves' floor, the translucency times the material's; y F0; z the anisotropy
RWTexture2D<float4> g_MaterialSpecular : register(u5); // Each material's specular colour at normal incidence, for the rays' hits (HitMaterialOf)
RWTexture2D<float4> g_SpecularTable : register(u6); // g_lut2's slices one below the other, which Direct3D 12's lamps read (direct_light.hlsl)

cbuffer Parameters : register(b0)
{
    float4 g_View[3];          // World to view, rows
    float4 g_ProjectionParam;  // cPSScene c21: 1 / P00, 1 / P11, B, A
    float2 g_Jitter;           // cPSScene c28.xy, in NDC
    uint2 g_Size;
    uint g_HasAlbedo;
    uint g_HasSpecular;
    uint g_HasTables;
    float g_SkyReflectionFloor; // The sun pass's cPSLight c2.z, the frame before
    uint g_HasSea;
};

// The texel a point sampler takes at (u, v), as the sun pass reads the material table.
float4 PointAt(Texture2D<float4> table, float u, float v)
{
    uint width, height;
    table.GetDimensions(width, height);
    return table.Load(int3(min(uint2(float2(u, v) * float2(width, height)), uint2(width, height) - 1), 0));
}

// The Fresnel table at (cosine, F0), as a linear clamped sampler takes it.
float FresnelAt(float cosine, float F0)
{
    uint width, height;
    g_Fresnel.GetDimensions(width, height);
    float2 texel = clamp(float2(cosine * width, F0 * height) - 0.5, 0, float2(width, height) - 1);
    uint2 low = uint2(texel);
    uint2 high = min(low + 1, uint2(width, height) - 1);
    float2 t = frac(texel);
    float above = lerp(g_Fresnel.Load(int3(low.x, low.y, 0)).x, g_Fresnel.Load(int3(high.x, low.y, 0)).x, t.x);
    float below = lerp(g_Fresnel.Load(int3(low.x, high.y, 0)).x, g_Fresnel.Load(int3(high.x, high.y, 0)).x, t.x);
    return lerp(above, below, t.y);
}

// Water's reflectance (F0 0.02) at the eye's angle, over GGX's lobe: Karis' fit of the split sum ("Physically
// Based Shading on Mobile", 2014).
float WaterReflectance(float toEyeN, float roughness)
{
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * toEyeN)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return 0.02 * ab.x + ab.y;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= g_Size)) return;

    if (g_HasTables && id.y == 0 && id.x < 256)
        g_MaterialSpecular[uint2(id.x, 0)] = float4(g_Materials.Load(int3(id.x, 0, 0)).rgb * FresnelAt(1, g_Materials.Load(int3(id.x, 1, 0)).x), 1);
    if (g_HasTables && id.x < 64 && id.y < 256) g_SpecularTable[id] = g_SpecularLobe.Load(int4(id.x, id.y % 16, id.y / 16, 0));

    float depth = g_Depth.Load(int3(id, 0));
    if (depth <= 0)
    {
        g_NormalRoughness[id] = 0;
        g_DiffuseAlbedo[id] = float4(0.5, 0.5, 0.5, 1);
        g_SpecularAlbedo[id] = 0;
        g_Material[id] = 0;
        g_Lobe[id] = 0;
        return;
    }

    // DecodeViewSpaceNormal, as the pack above.
    float3 encoded = g_Normal.Load(int3(id, 0)).xyz;
    float2 xy = encoded.xy * 2 - 1;
    float z = encoded.z * encoded.z * 2 - 1;
    float rest = 1 - z * z;
    float3 normal = float3(rsqrt(dot(xy, xy) * rest + 1e-7) * (rest * xy), z);
    float3 world = normalize(normal.x * g_View[0].xyz + normal.y * g_View[1].xyz + normal.z * g_View[2].xyz);

    // The eye's direction to the pixel in view space, as the sun pass works it out.
    float2 ndc = float2((id.x + 0.5) / g_Size.x * 2 - 1, 1 - (id.y + 0.5) / g_Size.y * 2) - g_Jitter;
    float viewZ = g_ProjectionParam.z / (depth - g_ProjectionParam.w);
    float3 eye = normalize(float3(ndc * g_ProjectionParam.xy * viewZ, viewZ));

    float4 specular = g_HasSpecular ? g_Specular.Load(int3(id, 0)) : float4(1, 0, 0, 0);
    float roughness = specular.x;
    float toEyeN = saturate(-dot(normal, eye));
    float3 specularColor = specular.y;
    float3 reflected = specular.y * max(0.04, g_SkyReflectionFloor);
    float F0 = 0.04, anisotropy = 0;
    float translucencyScale = 0; // The material's (its table's row 0.25, w)
    if (g_HasTables)
    {
        float4 color = PointAt(g_Materials, specular.z, 0.25);
        float4 row = PointAt(g_Materials, specular.z, 0.75);
        F0 = row.x;
        anisotropy = row.w;
        specularColor = specular.y * color.rgb;
        reflected = specularColor * FresnelAt(toEyeN, max(F0, g_SkyReflectionFloor));
        translucencyScale = color.w;
    }

    g_NormalRoughness[id] = float4(world, roughness);
    g_DiffuseAlbedo[id] = float4(g_HasAlbedo ? g_Albedo.Load(int3(id, 0)).rgb : 0.5, 1);
    bool isSea = g_HasSea && g_SeaDepth.Load(int3(id, 0)) == depth;
    g_SpecularAlbedo[id] = float4(isSea ? WaterReflectance(toEyeN, roughness) : saturate(reflected * ((1 - roughness) * 0.5)), isSea ? 0 : 1);
    g_Material[id] = float4(saturate(specularColor), specular.w);
    g_Lobe[id] = float4(saturate(specular.w * translucencyScale), F0, anisotropy, 0);
}
)";

struct GuideParameters
{
    float view[3][4];
    float projectionParam[4];
    float jitter[2];
    std::uint32_t width, height, hasAlbedo, hasSpecular, hasTables;
    float skyReflectionFloor;
    std::uint32_t hasSea, padding[3];
};

// The light the global illumination keeps in its caches is the game's before exposure, scaled by
// this: the game's auto-exposure changes quickly, and caches holding exposed light lagged behind it
// and overshot (a white screen). The output takes the frame's exposure (gi_renderer::Frame::outputScale).
constexpr float LightScale = 1.0f / 1024;
// The most radiance a self-lit mesh lights others with, in the game's units: the sun's light. Their intensities are set
// for how they look (up to 221,182, a bulb's bloom); their rooms are lit by the lamps placed beside them.
constexpr float MostEmission = 20000;

// The sky's light as PsWork holds it, before exposure, and the exposure the pass had; whether the
// probe's light stands in for it (the game gave no sky).
struct Sky
{
    std::array<std::array<float, 16>, 3> matrices{};
    float exposure{};
    bool fromProbe{};
};

// The sun's colour before exposure (cPSLight c1), and the exposure the sun pass had.
struct Sun
{
    std::array<float, 3> direction{}, color{};
    float exposure{};
    float skyReflectionFloor{}; // cPSLight c2.z
};

// What a deferred context, then its command list, recorded that the kick and the wait need.
struct Notes
{
    bool kick{}, wait{};
    ComPtr<ID3D11ShaderResourceView> depth, normals, albedo, velocity;
    std::optional<Sky> sky;
    std::optional<Sun> sun;
    // The game's shadow map range from the sun pass inside it: cPSLight c8.x, c8.y, c6.x, and 1.
    std::optional<std::array<float, 4>> shadowRange;

    void merge(Notes later)
    {
        kick |= later.kick;
        wait |= later.wait;
        if (later.depth) depth = std::move(later.depth);
        if (later.normals) normals = std::move(later.normals);
        if (later.albedo) albedo = std::move(later.albedo);
        if (later.velocity) velocity = std::move(later.velocity);
        if (later.sky) sky = later.sky;
        if (later.sun) sun = later.sun;
        if (later.shadowRange) shadowRange = later.shadowRange;
    }
};

// A shared texture with the views Direct3D 11 uses.
using SharedTexture = ComputeTexture;

FrameLatch<Options> latch;
// The scene runs (for the light, the shadows or the point and spot lights), the light runs, the sun's
// shadows run, the point and spot lights' shadows run.
std::atomic<bool> enabled{}, lighting{}, shadowing{}, localShadowing{}, asksPathTracing{}, failed{};

CommandListNotes<Notes> notes;

// ---- The immediate context's side, guarded by executionMutex ----
std::mutex executionMutex;
ComPtr<ID3D11Device5> device;
ComPtr<ID3D11DeviceContext4> immediate;
ID3D11DeviceContext* nativeImmediate{};
ComPtr<ID3D11Fence> fence;
HANDLE fenceHandle{};
std::uint64_t fenceValue{}, lightSignalled{}, lightWaited{};
std::uint32_t kicks{};
std::optional<SceneConstants> sceneCamera, previousCamera;
// The game's velocity for its moving meshes, whether a list that drew it ran since the last present and
// before the kick (only then does it hold this frame's), and its shared copy for the shadow denoiser.
ComPtr<ID3D11ShaderResourceView> objectVelocity;
bool objectVelocityDrawn{};
ComPtr<ID3D11Texture2D> objectVelocityCopy;
std::optional<Sky> sky;
// The last sun pass's sun, while each frame has one: a frame without (indoors) has no sun. Until
// 2026-10-26 it was kept, and the main menu's daytime sun lit a night-time hospital through its windows.
std::optional<Sun> sun;
bool sunSincePresent{};
std::array<float, 4> shadowRange{};
ComPtr<ID3D11ShaderResourceView> normals, albedo;
ComPtr<ID3D11ComputeShader> packShader, guideShader;
ComPtr<ID3D11Buffer> packParameters, guideParameters;
// DLSS Ray Reconstruction's guides at the render size, and the kick that made them; path tracing's material
// guides (guideMaterial, guideLobe), the materials' specular colours and the game's specular table with them,
// shared like the others.
ComputeTexture guideNormals, guideDiffuse, guideSpecular, guideMaterial, guideLobe, materialSpecular, specularTable;
std::uint32_t guidesKick{};
// The frames Ray Reconstruction denoised, counted by reconstructed(), and the count the last kick saw.
std::atomic<std::uint64_t> reconstructions{};
std::uint64_t reconstructionsAtKick{};
// Shared copies of the game's terrain heightmap (gi_renderer::Terrain).
ComPtr<ID3D11Texture2D> terrainPages, terrainHeights;
SharedTexture depthOut, normalOut, albedoOut;
std::string failure;
std::atomic<std::uint64_t> lastKick{}; // GetTickCount64 of the last frame lit
// Diagnostics: the kicks' CPU time in performance counter ticks, and the kicks, since the last log.
std::atomic<std::int64_t> kickTicks{};
std::atomic<std::uint64_t> kicksSinceLog{};
// And its steps' (the rest is the work after the renderer's); how often the terrain's converter
// constants and heightmap changed, and the converter last used.
enum KickStep { InputsStep, ModelsStep, TexturesStep, TerrainStep, SignalStep, RenderStep, KickSteps };
std::array<std::atomic<std::int64_t>, KickSteps> kickStepTicks{};
std::atomic<std::uint32_t> converterChanges{}, heightmapChanges{};
std::array<std::atomic<float>, 8> lastConverter{};
// Diagnostics: the kicks whose shadow denoiser had the game's velocity for its moving meshes, and not.
std::atomic<std::uint64_t> kicksWithObjectVelocity{}, kicksWithoutObjectVelocity{};
// Diagnostics: the kicks whose camera's viewport is smaller than the depth they pack, as a smaller view's
// is (the rays would trace from the intro's mirror); the contexts' or lists' notes and the cameras
// (gi_inputs) never handed over.
std::atomic<std::uint64_t> kicksFromSmallerView{}, expiredNotes{}, expiredCameras{};

// The light the resolve reads and the shadow the sun pass reads, made at the render size on
// whichever thread asks first. Taken after executionMutex when both are.
std::mutex lightMutex;
ComPtr<ID3D11Device> lightDevice;
SharedTexture output, shadowOutput, shadowDebugOutput, lightDebugOutput, reflectionMapOutput, reflectionOutput, reflectionDistanceOutput, forwardShadowOutput, lampDiffuseOutput,
    lampSpecularOutput;
// What the last kick traced, for the threads that read its results.
std::atomic<kick_traces::KickTraces> lastTraces{};
static_assert(std::atomic<kick_traces::KickTraces>::is_always_lock_free);
// depthOut's view, for the recording threads (surfaceDepth).
ComPtr<ID3D11ShaderResourceView> publishedDepth;
bool shadowWritten{}, shadowDebugWritten{}; // Direct3D 12 has traced into shadowOutput, shadowDebugOutput
bool lightDebugWritten{}; // And into lightDebugOutput

SharedTexture makeShared(ID3D11Device* creator, UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    return makeComputeTexture(creator, width, height, format, what, SharedWithD3D12);
}

std::uint64_t keyOf(ID3D11DeviceContext* context)
{
    return reinterpret_cast<std::uint64_t>(context);
}

// Call with executionMutex held.
void fail(const std::string& reason)
{
    failed = true;
    failure = reason;
    log("Global illumination stopped: " + reason);
    gi_inputs::setActive(false);
    engine_scene::setActive(false);
    gi_renderer::stop();
    gi_textures::reset();
}

// Call with executionMutex held.
void ensurePipeline()
{
    if (packShader)
    {
        if (!gi_renderer::started()) gi_renderer::start(device.Get(), fenceHandle);
        return;
    }

    packShader = computeShader(device.Get(), PackSource, "gi-pack");
    packParameters = makeConstantBuffer(device.Get(), sizeof(PackParameters), "Creating the G-buffer pack's constants");
    guideShader = computeShader(device.Get(), GuideSource, "reconstruction-guides");
    guideParameters = makeConstantBuffer(device.Get(), sizeof(GuideParameters), "Creating the Ray Reconstruction guides' constants");

    check(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), "Creating the shared fence");
    check(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle), "Sharing the fence");
    gi_renderer::start(device.Get(), fenceHandle);
}

// The G-buffer into the shared textures, on the immediate context; call with executionMutex held.
void pack(ID3D11ShaderResourceView* depth, const SceneConstants& scene)
{
    const auto [width, height] = sizeOf(depth);
    if (depthOut.width != width || depthOut.height != height)
    {
        depthOut = makeShared(device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, "Creating the shared depth");
        {
            std::lock_guard lock(lightMutex);
            publishedDepth = depthOut.view;
        }

        normalOut = makeShared(device.Get(), width, height, DXGI_FORMAT_R10G10B10A2_UNORM, "Creating the shared normals");
        albedoOut = makeShared(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the shared albedo");
        log("Global illumination at " + std::to_string(width) + "x" + std::to_string(height) + ".");
    }

    const bool hasAlbedo = albedo && sizeOf(albedo.Get()) == std::pair{width, height};
    PackParameters parameters{};
    for (int row = 0; row < 3; ++row) std::memcpy(parameters.view[row], scene.view[row], sizeof(parameters.view[row]));
    parameters.width = width;
    parameters.height = height;
    parameters.hasAlbedo = hasAlbedo;
    const auto specular = indirect_light_pass::specularTarget(width, height);
    parameters.hasSpecular = specular != nullptr;
    check(write(immediate.Get(), packParameters.Get(), parameters), "Writing the G-buffer pack's constants");

    ID3D11ShaderResourceView* inputs[]{depth, normals.Get(), hasAlbedo ? albedo.Get() : nullptr, specular.Get()};
    ID3D11UnorderedAccessView* outputs[]{depthOut.access.Get(), normalOut.access.Get(), albedoOut.access.Get()};
    immediate->CSSetShader(packShader.Get(), nullptr, 0);
    immediate->CSSetConstantBuffers(0, 1, packParameters.GetAddressOf());
    immediate->CSSetShaderResources(0, 4, inputs);
    immediate->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
    immediate->Dispatch(groups(width), groups(height), 1);
    ID3D11ShaderResourceView* noInputs[4]{};
    ID3D11UnorderedAccessView* noOutputs[3]{};
    immediate->CSSetShaderResources(0, 4, noInputs);
    immediate->CSSetUnorderedAccessViews(0, 3, noOutputs, nullptr);
    immediate->CSSetShader(nullptr, nullptr, 0);
}

// DLSS Ray Reconstruction's guides from the G-buffer, at the depth's size; on the immediate context, call
// with executionMutex held.
void makeGuides(ID3D11ShaderResourceView* depth, const SceneConstants& scene, float skyReflectionFloor)
{
    const auto [width, height] = sizeOf(depth);
    if (guideNormals.width != width || guideNormals.height != height)
    {
        // Ray Reconstruction reads them on Direct3D 12 (ray_reconstruction.hpp).
        guideNormals = makeShared(device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the Ray Reconstruction normals");
        guideDiffuse = makeShared(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the Ray Reconstruction diffuse albedo");
        guideSpecular = makeShared(device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the Ray Reconstruction specular albedo");
        guideMaterial = makeShared(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating path tracing's material guide");
        guideLobe = makeShared(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating path tracing's lobe guide");
        materialSpecular = makeShared(device.Get(), 256, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the materials' specular colours");
        specularTable = makeShared(device.Get(), 64, 256, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the game's specular table");
    }

    const auto specular = indirect_light_pass::specularTarget(width, height);
    const auto tables = indirect_light_pass::materialTables();
    const bool hasAlbedo = albedo && sizeOf(albedo.Get()) == std::pair{width, height};
    GuideParameters parameters{};
    for (int row = 0; row < 3; ++row) std::memcpy(parameters.view[row], scene.view[row], sizeof(parameters.view[row]));
    std::memcpy(parameters.projectionParam, scene.projectionParam, sizeof(parameters.projectionParam));
    parameters.jitter[0] = scene.cameraCenterOffset[0];
    parameters.jitter[1] = scene.cameraCenterOffset[1];
    parameters.width = width;
    parameters.height = height;
    parameters.hasAlbedo = hasAlbedo;
    parameters.hasSpecular = specular != nullptr;
    parameters.hasTables = tables.lobe && tables.fresnel && tables.materials;
    parameters.skyReflectionFloor = skyReflectionFloor;
    const auto seaDepth = sea::surfaceDepth(width, height);
    parameters.hasSea = seaDepth != nullptr;
    check(write(immediate.Get(), guideParameters.Get(), parameters), "Writing the Ray Reconstruction guides' constants");

    ID3D11ShaderResourceView* inputs[]{depth, normals.Get(), hasAlbedo ? albedo.Get() : nullptr, specular.Get(), tables.fresnel.Get(), tables.materials.Get(),
        seaDepth.Get(), tables.lobe.Get()};
    ID3D11UnorderedAccessView* outputs[]{guideNormals.access.Get(), guideDiffuse.access.Get(), guideSpecular.access.Get(), guideMaterial.access.Get(),
        guideLobe.access.Get(), materialSpecular.access.Get(), specularTable.access.Get()};
    immediate->CSSetShader(guideShader.Get(), nullptr, 0);
    immediate->CSSetConstantBuffers(0, 1, guideParameters.GetAddressOf());
    immediate->CSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
    immediate->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(outputs)), outputs, nullptr);
    immediate->Dispatch(groups(width), groups(height), 1);
    ID3D11ShaderResourceView* noInputs[std::size(inputs)]{};
    ID3D11UnorderedAccessView* noOutputs[std::size(outputs)]{};
    immediate->CSSetShaderResources(0, static_cast<UINT>(std::size(noInputs)), noInputs);
    immediate->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(noOutputs)), noOutputs, nullptr);
    immediate->CSSetShader(nullptr, nullptr, 0);
}

// Copies the most detailed level of a view's texture into a shared copy for Direct3D 12, made again
// when its size or format changes; the view's format for Direct3D 12's view. Call with executionMutex held.
bool copyShared(ID3D11ShaderResourceView* view, ComPtr<ID3D11Texture2D>& copy, DXGI_FORMAT& format)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    view->GetDesc(&viewDesc);
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> source;
    view->GetResource(resource.GetAddressOf());
    if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || FAILED(resource.As(&source))) return false;

    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const UINT mip = viewDesc.Texture2D.MostDetailedMip;
    D3D11_TEXTURE2D_DESC made{};
    made.Width = std::max(desc.Width >> mip, 1u);
    made.Height = std::max(desc.Height >> mip, 1u);
    made.MipLevels = made.ArraySize = 1;
    made.Format = desc.Format;
    made.SampleDesc.Count = 1;
    made.Usage = D3D11_USAGE_DEFAULT;
    made.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    made.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    D3D11_TEXTURE2D_DESC have{};
    if (copy) copy->GetDesc(&have);
    if (!copy || have.Width != made.Width || have.Height != made.Height || have.Format != made.Format)
    {
        copy.Reset();
        if (FAILED(device->CreateTexture2D(&made, nullptr, copy.GetAddressOf()))) return false;
    }

    immediate->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, source.Get(), D3D11CalcSubresource(mip, 0, desc.MipLevels), nullptr);
    format = viewDesc.Format;
    return true;
}

// The game's terrain heightmap into its shared copies, and its constants, for the frame. Call with
// executionMutex held.
void copyTerrain(const engine_scene::TerrainInputs& inputs, gi_renderer::Frame& frame)
{
    static std::array<ID3D11ShaderResourceView*, 2> lastViews{};
    if (const std::array views{inputs.pages.Get(), inputs.heights.Get()}; views != lastViews)
    {
        if (lastViews[0]) ++heightmapChanges;
        lastViews = views;
    }

    static std::optional<std::array<float, 8>> previous;
    if (previous && *previous != inputs.converter) ++converterChanges;
    previous = inputs.converter;
    for (std::size_t i = 0; i < 8; ++i) lastConverter[i] = inputs.converter[i];

    gi_renderer::Terrain terrain;
    if (!copyShared(inputs.pages.Get(), terrainPages, terrain.pagesFormat) || !copyShared(inputs.heights.Get(), terrainHeights, terrain.heightsFormat))
        return;

    terrain.pages = terrainPages.Get();
    terrain.heights = terrainHeights.Get();
    std::copy_n(inputs.scaleOffset.begin(), 4, terrain.scale.begin());
    std::copy_n(inputs.scaleOffset.begin() + 4, 4, terrain.offset.begin());
    std::copy_n(inputs.converter.begin(), 4, terrain.uvConverter.begin());
    std::copy_n(inputs.converter.begin() + 4, 4, terrain.texelCorrect.begin());
    frame.terrain = terrain;
}

gi_renderer::Matrix matrixOf(const float (&rows)[4][4])
{
    gi_renderer::Matrix matrix{};
    static_assert(sizeof(matrix) == sizeof(rows));
    std::memcpy(matrix.data(), rows, sizeof(matrix));

    return matrix;
}

// Diagnostics: the light the scene gets, every ten seconds.
void logLights(const gi_renderer::Frame& frame, float cameraExposure)
{
    static std::uint64_t lastLightLog{};
    const std::uint64_t now = lastKick;
    if (!frame.traces.tracesLight || now - lastLightLog <= 10'000) return;

    lastLightLog = now;
    const auto text = [](const std::array<float, 3>& v) { return std::to_string(v[0]) + " " + std::to_string(v[1]) + " " + std::to_string(v[2]); };
    std::string line = "Global illumination's lights (x 1/1024, before exposure): sun " + text(frame.sunColor) + " towards "
        + text(frame.sunDirection) + "; exposure: main view " + std::to_string(cameraExposure) + ", sun pass "
        + std::to_string(sun ? sun->exposure : 0.0f) + ", probe pass " + std::to_string(sky ? sky->exposure : 0.0f);
    if (sky)
    {
        gi_lighting::SphericalHarmonics radiance{};
        for (int i = 0; i < 9; ++i) radiance[i] = frame.skyRadiance[i];
        // The game's own sky light on an upward surface: c = (0, 0, 1, 1) for n = (0, 1, 0).
        std::array<float, 3> gameUp{};
        for (int color = 0; color < 3; ++color)
        {
            const auto& m = sky->matrices[color];
            gameUp[color] = LightScale * (m[10] + m[11] + m[14] + m[15]);
        }

        line += "; sky radiance up " + text(gi_lighting::evaluate(radiance, {0, 1, 0})) + ", sideways " + text(gi_lighting::evaluate(radiance, {1, 0, 0}))
            + ", down " + text(gi_lighting::evaluate(radiance, {0, -1, 0})) + "; the game's sky light up " + text(gameUp)
            + (sky->fromProbe ? " (no sky from the game: its probe's light)" : "");
    }

    log(line + ".");
}

// Diagnostics: what the scene holds, and the CPU work a frame, every ten seconds while it runs.
void logStatus()
{
    static std::uint64_t lastStatus{};
    const auto now = GetTickCount64();
    if (now - lastStatus <= 10'000) return;

    lastStatus = now;
    log("Global illumination: " + engine_scene::poseTiming() + ".");
    log("Global illumination: " + status() + "; the denoisers had the game's velocity for moving meshes in "
        + std::to_string(kicksWithObjectVelocity.exchange(0)) + " frames, not in " + std::to_string(kicksWithoutObjectVelocity.exchange(0)) + "; "
        + std::to_string(kicksFromSmallerView.exchange(0)) + " kicks from a camera smaller than their depth; " + std::to_string(expiredNotes.exchange(0))
        + " contexts' or lists' notes and " + std::to_string(expiredCameras.exchange(0)) + " cameras expired");

    const auto work = gi_inputs::takeWork();
    const auto capture = constant_capture::takeWork();
    const double frames = static_cast<double>(std::max<std::uint64_t>(kicksSinceLog.exchange(0), 1));
    const auto milliseconds = [](std::atomic<std::int64_t>& ticks) { return clock::millisecondsOf(ticks.exchange(0)); };
    const auto perFrame = [&](double value) { return std::to_string(value / frames); };
    const double kick = milliseconds(kickTicks);
    std::string steps;
    for (const auto [step, name] : {std::pair{InputsStep, "inputs"}, {ModelsStep, "models"}, {TexturesStep, "textures"}, {TerrainStep, "terrain"},
             {SignalStep, "signal"}, {RenderStep, "renderer"}})
        steps += std::string(steps.empty() ? "" : ", ") + name + " " + perFrame(milliseconds(kickStepTicks[step]));
    steps += "; the terrain's converter changed " + std::to_string(converterChanges.exchange(0)) + " times (now";
    for (const auto& value : lastConverter) steps += " " + std::to_string(value.load());
    steps += "), its heightmap textures " + std::to_string(heightmapChanges.exchange(0)) + " times";

    // The hooks: the draws' and the constant capture's unmaps.
    log("Global illumination's CPU time a frame: hooks " + perFrame(work.hookMilliseconds + capture.unmapMilliseconds) + " ms (all threads) over "
        + perFrame(static_cast<double>(work.draws)) + " mesh draws; the kick " + perFrame(kick) + " ms (render thread: " + steps + "); "
        + std::to_string(capture.refusedReads) + " constant reads refused (a write of another size).");
}

// Remakes `texture` at this size unless it has it; whether it did.
bool resizeShared(SharedTexture& texture, ID3D11Device* creator, UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    if (texture.width == width && texture.height == height) return false;

    texture = makeShared(creator, width, height, format, what);
    return true;
}

// Milliseconds since the last frame lit; the most there is before the first.
std::uint64_t sinceKick()
{
    const auto last = lastKick.load(std::memory_order_relaxed);
    return last ? GetTickCount64() - last : UINT64_MAX;
}

// The last kick's path tracing, while its results are fresh.
bool wasPathTraced() noexcept
{
    return lastTraces.load(std::memory_order_relaxed).isPathTraced && !failed && sinceKick() < 1000;
}

// The frame's kick, before the list with the probe upscale runs; call with executionMutex held.
void kick(ID3D11ShaderResourceView* depth, const Options& options)
{
    LARGE_INTEGER stepStarted{};
    QueryPerformanceCounter(&stepStarted);
    const auto endStep = [&](KickStep step) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        kickStepTicks[step] += now.QuadPart - stepStarted.QuadPart;
        stepStarted = now;
    };

    ensurePipeline();
    // Direct3D 12 is done with the shared inputs of the frame before.
    performance::Timed timed(immediate.Get(), performance::Pass::GiWait);
    if (lightSignalled) check(immediate->Wait(fence.Get(), lightSignalled), "Waiting for the global illumination");

    timed.next(performance::Pass::GiInputs);
    if (const auto taken = gi_inputs::takeCamera())
    {
        previousCamera = sceneCamera ? sceneCamera : taken;
        sceneCamera = taken;
    }

    if (!sceneCamera || !depth || !normals) return;


    if constexpr (DiagnosticLog)
    {
        const auto [width, height] = sizeOf(depth);
        if (sceneCamera->width < static_cast<float>(width) || sceneCamera->height < static_cast<float>(height)) ++kicksFromSmallerView;
    }

    pack(depth, *sceneCamera);
    const auto& asked = options.asked;
    if (asked.asksRayReconstruction) makeGuides(depth, *sceneCamera, sun ? sun->skyReflectionFloor : 0);
    const auto reconstructedFrames = reconstructions.load(std::memory_order_relaxed);
    const bool isDenoisedByReconstruction = reconstructedFrames != reconstructionsAtKick;

    // The point and spot lights recorded for this frame, a bit each in their shadow mask. Path tracing lights
    // them itself, with their masks.
    std::vector<local_lights::TracedLight> tracedLights;
    if (kick_traces::takesLights(asked, isDenoisedByReconstruction)) tracedLights = local_lights::takeForTrace(*sceneCamera);
    forward_surfaces::SurfaceTextures surfaces;
    if (kick_traces::isPathTraced(asked, isDenoisedByReconstruction)) surfaces = forward_surfaces::beginFrame(immediate.Get(), depthOut.width, depthOut.height);
    const auto traces = kick_traces::tracesOf(asked,
        {.isDenoisedByReconstruction = isDenoisedByReconstruction,
            .hasSun = sun.has_value(),
            .hasLights = !tracedLights.empty(),
            .hasMaterialTables = indirect_light_pass::materialTables().lobe != nullptr,
            .hasGlassTargets = surfaces.reflection != nullptr});

    ID3D11Texture2D* light{};
    ID3D11Texture2D* shadow{};
    ID3D11Texture2D* shadowDebug{};
    ID3D11Texture2D* lightDebug{};
    {
        std::lock_guard lock(lightMutex);
        resizeShared(output, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the light");
        light = output.texture.Get();
        if (asked.runsGlobalIllumination && options.lightDebugView != gi_renderer::LightDebugView::None)
        {
            if (resizeShared(lightDebugOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the light's debug view"))
                lightDebugWritten = false;

            lightDebug = lightDebugOutput.texture.Get();
        }
        if (traces.tracesSun)
        {
            if (resizeShared(shadowOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16_FLOAT, "Creating the shadow")) shadowWritten = false;
            shadow = shadowOutput.texture.Get();
            if (options.debugShadows)
            {
                if (resizeShared(shadowDebugOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the shadow debug view"))
                    shadowDebugWritten = false;

                shadowDebug = shadowDebugOutput.texture.Get();
            }
        }
    }

    ID3D11Texture2D* reflection{};
    ID3D11Texture2D* reflectionDistance{};
    ID3D11Texture2D* reflectionMap{};
    ID3D11Texture2D* forwardShadowTexture{};
    if (traces.isPathTraced)
    {
        std::lock_guard lock(lightMutex);
        resizeShared(reflectionOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the reflections");
        resizeShared(reflectionDistanceOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16_FLOAT, "Creating the reflections' hit distance");
        resizeShared(reflectionMapOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the reflection map");
        reflection = reflectionOutput.texture.Get();
        reflectionDistance = reflectionDistanceOutput.texture.Get();
        reflectionMap = reflectionMapOutput.texture.Get();
        resizeShared(forwardShadowOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16_FLOAT, "Creating the forward surfaces' shadow");
        forwardShadowTexture = forwardShadowOutput.texture.Get();
    }

    auto* lampMasks = traces.isPathTraced ? path_tracing::packMasks(immediate.Get(), tracedLights) : nullptr;
    std::vector<gi_renderer::LocalLight> localLightsToTrace;
    for (const auto& each : tracedLights) localLightsToTrace.push_back(each.light);

    ID3D11Texture2D* lampDiffuse{};
    ID3D11Texture2D* lampSpecular{};
    if (traces.tracesLampLight)
    {
        std::lock_guard lock(lightMutex);
        resizeShared(lampDiffuseOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the lamps' diffuse light");
        resizeShared(lampSpecularOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the lamps' specular light");
        lampDiffuse = lampDiffuseOutput.texture.Get();
        lampSpecular = lampSpecularOutput.texture.Get();
    }

    const trace::Kick traceKick{device.Get(), depthOut.width, depthOut.height, traces};
    for (auto* each : trace::inOrder()) each->gather(traceKick);

    // The game's models, the textures their meshes use and its terrain, copied before Direct3D 12 may
    // read them.
    endStep(InputsStep);
    gi_renderer::Frame frame;
    engine_scene::Snapshot engineScene;
    engine_scene::take(engineScene, gi_inputs::takeDrawnBones(*sceneCamera));
    for (const auto& model : engineScene.newModels)
        for (const auto& part : model->parts)
        {
            const bool isGlass = part.role == scene_description::MeshRole::Glass;
            gi_textures::want(part.baseTexture, isGlass ? gi_textures::Use::Data : scene_description::isCutOut(part.role) ? gi_textures::Use::CutOut : gi_textures::Use::Colour);
            gi_textures::want(part.specularTexture, gi_textures::Use::Data);
            gi_textures::want(part.normalTexture, gi_textures::Use::Data);
        }
    endStep(ModelsStep);
    gi_textures::update(device.Get(), immediate.Get(), frame.newTextures);
    endStep(TexturesStep);
    if (const auto terrain = engine_scene::takeTerrain()) copyTerrain(*terrain, frame);
    frame.seaLevel = sea::level();
    auto* skyCube = traces.isPathTraced ? sky_capture::update(immediate.Get(), LightScale) : nullptr;

    // The game's velocity for its moving meshes, for the shadows' and the light's denoisers: only this
    // frame's, drawn before this list, at the render size.
    DXGI_FORMAT velocityFormat{};
    const bool wantsVelocity = traces.tracesSun || traces.tracesLight;
    const bool velocityCopied = wantsVelocity && objectVelocityDrawn && objectVelocity && sizeOf(objectVelocity.Get()) == sizeOf(depth)
        && copyShared(objectVelocity.Get(), objectVelocityCopy, velocityFormat);
    if (wantsVelocity) ++(velocityCopied ? kicksWithObjectVelocity : kicksWithoutObjectVelocity);
    endStep(TerrainStep);
    const auto inputsReady = ++fenceValue;
    check(immediate->Signal(fence.Get(), inputsReady), "Signalling the global illumination's inputs");
    immediate->Flush();
    endStep(SignalStep);

    frame.textures = {.depth = depthOut.texture.Get(),
        .normal = normalOut.texture.Get(),
        .albedo = albedoOut.texture.Get(),
        .output = light,
        .shadow = shadow,
        .shadowDebug = shadowDebug,
        .velocity = velocityCopied ? objectVelocityCopy.Get() : nullptr,
        .velocityFormat = velocityFormat,
        .lightDebug = lightDebug};
    if (traces.isPathTraced)
    {
        auto& textures = frame.textures;
        textures.guideNormals = guideNormals.texture.Get();
        textures.guideSpecular = guideSpecular.texture.Get();
        textures.reflection = reflection;
        textures.reflectionDistance = reflectionDistance;
        textures.reflectionMap = reflectionMap;
        textures.guideMaterial = guideMaterial.texture.Get();
        textures.guideLobe = guideLobe.texture.Get();
        textures.specularTable = specularTable.texture.Get();
        textures.lampMasks = lampMasks;
        textures.lampDiffuse = lampDiffuse;
        textures.lampSpecular = lampSpecular;
        textures.materialSpecular = materialSpecular.texture.Get();
        textures.skyCube = skyCube;
        textures.surfaceDepth = surfaces.depth;
        textures.surfaceNormal = surfaces.normal;
        textures.surfaceReflection = surfaces.reflection;
        textures.forwardShadow = forwardShadowTexture;
    }

    frame.width = depthOut.width;
    frame.height = depthOut.height;

    const auto view = camera::viewOf(*sceneCamera);
    frame.viewProjection = matrixOf(sceneCamera->projectionView);
    frame.previousViewProjection = matrixOf(previousCamera->projectionView);
    frame.view = matrixOf(sceneCamera->view);
    frame.projection = matrixOf(sceneCamera->projection);
    frame.previousView = matrixOf(previousCamera->view);
    frame.previousProjection = matrixOf(previousCamera->projection);
    frame.cameraCut = camera::cameraCut(camera::viewOf(*previousCamera), view);
    frame.jitter = {sceneCamera->cameraCenterOffset[0], sceneCamera->cameraCenterOffset[1], previousCamera->cameraCenterOffset[0],
        previousCamera->cameraCenterOffset[1]};
    frame.eye = view.eye;
    frame.nearPlane = view.nearPlane;
    frame.farPlane = view.farPlane;
    // P11 is 1 / tan(half the vertical field of view).
    frame.verticalFov = 2 * std::atan(1 / std::max(std::abs(view.focal), 1e-3f));
    if (sun)
    {
        frame.sunDirection = sun->direction;
        frame.sunColor = {sun->color[0] * LightScale, sun->color[1] * LightScale, sun->color[2] * LightScale};
    }

    if (sky)
    {
        const auto radiance = gi_lighting::skyRadiance(sky->matrices, LightScale);
        for (int i = 0; i < 9; ++i) frame.skyRadiance[i] = radiance[i];
    }

    // The main view's exposure, as this frame's lighting passes have it; the sun pass's before the main
    // view's has one.
    const float cameraExposure = sceneCamera->exposure;
    const float exposure = cameraExposure > 0 ? cameraExposure : sun ? sun->exposure : 0;
    frame.outputScale = exposure / LightScale;

    frame.newModels = std::move(engineScene.newModels);
    frame.modelInstances = std::move(engineScene.instances);
    frame.characters = std::move(engineScene.characters);
    frame.modelInstancesUpdated = engineScene.instancesUpdated;
    frame.frameIndex = ++kicks;

    // The water's caustics unproject their pixels with the camera the forward pass draws with.
    {
        forward_shadows::Cameras cameras{};
        const auto projectionView = matrixOf(sceneCamera->projectionView);
        camera::Matrix matrix{};
        for (std::size_t index = 0; index < matrix.size(); ++index) matrix[index] = static_cast<double>(projectionView[index]);
        const auto inverse = camera::inverse(matrix);
        for (std::size_t index = 0; index < inverse.size(); ++index) cameras.viewProjectionInverse[index] = static_cast<float>(inverse[index]);
        std::memcpy(cameras.eye, sceneCamera->eye, sizeof(cameras.eye));
        cameras.dimensions[0] = static_cast<float>(depthOut.width);
        cameras.dimensions[1] = static_cast<float>(depthOut.height);
        cameras.frame = static_cast<float>(frame.frameIndex);
        cameras.nearFar[0] = view.nearPlane;
        cameras.nearFar[1] = view.farPlane;
        forward_shadows::setCameras(cameras);
    }
    if (asked.asksRayReconstruction) guidesKick = kicks;
    frame.traces = traces;
    frame.emissionScale = traces.isPathTraced ? options.emission * LightScale : 0;
    frame.emissionCap = MostEmission * LightScale;
    reconstructionsAtKick = reconstructedFrames;
    lastTraces = traces;
    frame.showScene = asked.showsScene;
    frame.logOften = options.logOften;
    // Without the shadow map's shadow the rays have no range to leave anything to (BeyondShadowMap).
    // Path tracing's sun reads the rays' shadow alone (sun_light_pass.cpp): nothing is left to the shadow map.
    frame.shadowRange = options.withShadowMap && !traces.isPathTraced ? shadowRange : std::array<float, 4>{};
    frame.sunTangent = std::tan(options.sunSize * 0.5f * 3.14159265f / 180);
    frame.debugShadows = shadowDebug != nullptr;
    frame.shadowStarts = options.shadowStarts;
    frame.twoSidedShadows = options.twoSidedShadows;
    frame.lightDebugView = options.lightDebugView;
    frame.lightRays = static_cast<std::uint32_t>(std::clamp(options.lightPixelRays, 1, 4));
    // The lights' colours in the sun's units for the hits (gi_light.hlsl, LocalLightAt).
    for (auto& localLight : localLightsToTrace)
        for (auto& value : localLight.color) value *= LightScale;
    frame.localLights = std::move(localLightsToTrace);
    frame.localLightRays = static_cast<std::uint32_t>(options.lightRays);
    frame.probeLights = options.debugShadows;
    frame.waitValue = inputsReady;
    frame.signalValue = ++fenceValue;
    const auto result = gi_renderer::render(frame);
    endStep(RenderStep);
    lightSignalled = frame.signalValue;
    {
        std::lock_guard lock(lightMutex);
        lightDebugWritten = lightDebug && traces.tracesLight && lightDebugOutput.texture.Get() == lightDebug;
    }
    if (traces.tracesSun)
    {
        std::lock_guard lock(lightMutex);
        shadowWritten = shadowOutput.texture.Get() == shadow;
        shadowDebugWritten = shadowDebug && shadowDebugOutput.texture.Get() == shadowDebug;
    }
    engine_scene::forget(result.releasedModels);
    gi_textures::release(result.releasedTextures);
    lastKick = GetTickCount64();
    if constexpr (DiagnosticLog) logLights(frame, cameraExposure);
}
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* gameImmediate) noexcept
{
    {
        std::lock_guard lock(executionMutex);
        nativeImmediate = gameImmediate;
        if (FAILED(gameDevice->QueryInterface(IID_PPV_ARGS(&device))) || FAILED(gameImmediate->QueryInterface(IID_PPV_ARGS(&immediate))))
        {
            device.Reset();
            immediate.Reset();
            failed = true;
            failure = "Direct3D 11.4 is missing.";
        }
    }

    std::lock_guard lock(lightMutex);
    lightDevice = gameDevice;
}

void detach() noexcept
{
    enabled = false;
    gi_inputs::setActive(false);
    engine_scene::setActive(false);
    gi_inputs::detach();
    {
        std::lock_guard lock(executionMutex);
        gi_renderer::stop();
        for (auto* each : trace::inOrder()) each->detach();
        packShader.Reset();
        packParameters.Reset();
        guideShader.Reset();
        guideParameters.Reset();
        guideNormals = guideDiffuse = guideSpecular = guideMaterial = guideLobe = materialSpecular = specularTable = {};
        guidesKick = 0;
        terrainPages.Reset();
        terrainHeights.Reset();
        gi_textures::reset();
        depthOut = normalOut = albedoOut = {};
        normals.Reset();
        albedo.Reset();
        objectVelocity.Reset();
        objectVelocityCopy.Reset();
        objectVelocityDrawn = false;
        fence.Reset();
        if (fenceHandle) CloseHandle(fenceHandle);
        fenceHandle = nullptr;
        fenceValue = lightSignalled = lightWaited = 0;
        sceneCamera.reset();
        previousCamera.reset();
        sky.reset();
        sun.reset();
        shadowRange = {};
        immediate.Reset();
        nativeImmediate = nullptr;
        device.Reset();
        failed = false;
        failure.clear();
    }

    {
        std::lock_guard lock(lightMutex);
        output = shadowOutput = shadowDebugOutput = lightDebugOutput = reflectionOutput = reflectionDistanceOutput = reflectionMapOutput = lampDiffuseOutput =
            lampSpecularOutput = {};
        lastTraces.store({});
        publishedDepth.Reset();
        shadowWritten = shadowDebugWritten = lightDebugWritten = false;
        lightDevice.Reset();
    }

    notes.clear();
}

void configure(const Configuration& configuration) noexcept
{
    const auto& settings = configuration.settings;
    const auto& diagnostics = configuration.diagnostics;
    const Options options{
        .asked = kick_traces::askedBy(configuration),
        .withShadowMap = settings.rayTracedShadows.withGameShadows,
        .lightRays = settings.rayTracedLights.shadowRaysPerPixel,
        .sunSize = settings.sun.diameterDegrees,
        .lightPixelRays = settings.globalIllumination.raysPerLightPixel,
        .debugShadows = diagnostics.debugView == DebugView::ShadowRays || diagnostics.debugView == DebugView::ShadowStarts,
        .shadowStarts = diagnostics.debugView == DebugView::ShadowStarts,
        .twoSidedShadows = diagnostics.twoSidedShadows,
        .lightDebugView = diagnostics.debugView == DebugView::LightHistory      ? gi_renderer::LightDebugView::History
                          : diagnostics.debugView == DebugView::LightValidation ? gi_renderer::LightDebugView::Validation
                                                                                : gi_renderer::LightDebugView::None,
        .logOften = diagnostics.logGlobalIlluminationOften,
        .emission = settings.pathTracing.emission,
    };

    latch.set(options);
}

Health health()
{
    if (!latch.latest().asked.needsScene()) return {};

    std::lock_guard lock(executionMutex);
    if (failed) return Health::unavailable("Ray tracing is unavailable: " + failure);
    if (sinceKick() > 1000) return Health::waiting("Ray tracing is waiting for the game's 3D view.");

    return {};
}

std::string status()
{
    std::string text = engine_scene::status() + "; " + gi_textures::status();
    const auto scene = gi_renderer::status();
    if (!scene.empty()) text += "\n" + scene;

    return text;
}

void vertexShaderBound(ID3D11DeviceContext* context, game_shaders::VertexShader kind) noexcept
{
    gi_inputs::vertexShaderBound(context, kind);
}

void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (call.indexed)
    {
        gi_inputs::drawIndexed(context, call);
        return;
    }

    if (call.count != 3 || call.instances != 1) return;

    try
    {
        const auto* row = shader.row();
        if (!row) return;

        if (row->pass == game_shaders::GamePass::CameraVelocity)
        {
            ComPtr<ID3D11ShaderResourceView> velocity;
            context->PSGetShaderResources(ObjectVelocitySlot, 1, velocity.GetAddressOf());
            notes.note(keyOf(context), [&](Notes& contextNotes) { contextNotes.velocity = std::move(velocity); });
            return;
        }

        if (row->pass != game_shaders::GamePass::SkyProbe) return;

        // PsWork (SH_SphereMap_ps): the probe's light and the sky's; the scene's exposure. The game's
        // ambient light is the probe's plus the sky's times the occlusion. Where the game gives no sky
        // (Mother Base: sky and occlusion zero, the probe's light all of it), the probe's light stands in
        // for the sky.
        const auto scene = constant_capture::pixelShaderReads<SceneConstants>(context);
        const auto probes = constant_capture::pixelShaderReads<game_constants::ProbeConstants>(context);
        if (!scene || !probes) return;

        Sky noted;
        const auto isZero = [](float value) { return value == 0.0f; };
        noted.fromProbe = std::ranges::all_of(probes->paramSHSky, [&](const float (&matrix)[16]) { return std::ranges::all_of(matrix, isZero); });
        const auto& light = noted.fromProbe ? probes->paramSH : probes->paramSHSky;
        for (int color = 0; color < 3; ++color) std::memcpy(noted.matrices[color].data(), light[color], sizeof(light[color]));
        noted.exposure = scene->exposure;
        notes.note(keyOf(context), [&](Notes& contextNotes) { contextNotes.sky = noted; });
    }
    catch (...)
    {
    }
}

void noteNormals(ID3D11DeviceContext* context, ID3D11ShaderResourceView* view) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    try
    {
        notes.note(keyOf(context), [&](Notes& contextNotes) { contextNotes.normals = view; });
    }
    catch (...)
    {
    }
}

void noteUpscale(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    try
    {
        notes.note(keyOf(context), [&](Notes& contextNotes) {
            contextNotes.kick = true;
            contextNotes.depth = depth;
        });
    }
    catch (...)
    {
    }
}

void noteSun(ID3D11DeviceContext* context, bool shadowMapped) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    try
    {
        const auto scene = constant_capture::pixelShaderReads<SceneConstants>(context);
        const auto light = constant_capture::pixelShaderReads<game_constants::SunLightConstants>(context);
        if (!scene || !light) return;

        Sun noted;
        noted.direction = game_constants::toWorld(*scene, light->direction, 0);
        const auto& direction = noted.direction;
        const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
        if (length > 0)
            for (auto& value : noted.direction) value /= length;

        noted.color = {light->color[0], light->color[1], light->color[2]};
        noted.exposure = scene->exposure;
        noted.skyReflectionFloor = light->skyReflectionFloor;
        notes.note(keyOf(context), [&](Notes& contextNotes) {
            contextNotes.sun = noted;
            // The range as SunShadow reads it (sun_light_pass_shaders.hpp).
            if (shadowMapped) contextNotes.shadowRange = std::array<float, 4>{light->shadowRangeCentre, light->shadowRangeStart, light->shadowRangeFade, 1};
            // The sun pass reads the shadows, and the point and spot lights are drawn before its list: it waits for them.
            if (shadowing.load(std::memory_order_relaxed) || localShadowing.load(std::memory_order_relaxed)) contextNotes.wait = true;
        });
    }
    catch (...)
    {
    }
}

void noteSkySpecular(ID3D11DeviceContext* context) noexcept
{
    if (!enabled.load(std::memory_order_relaxed) || !localShadowing.load(std::memory_order_relaxed)) return;

    try
    {
        notes.note(keyOf(context), [](Notes& contextNotes) { contextNotes.wait = true; });
    }
    catch (...)
    {
    }
}

void noteResolve(ID3D11DeviceContext* context, ID3D11ShaderResourceView* view) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    try
    {
        notes.note(keyOf(context), [&](Notes& contextNotes) {
            contextNotes.wait = true;
            contextNotes.albedo = view;
        });
    }
    catch (...)
    {
    }
}

ID3D11ShaderResourceView* light(UINT width, UINT height) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return nullptr;

    try
    {
        std::lock_guard lock(lightMutex);
        if (!lightDevice) return nullptr;
        resizeShared(output, lightDevice.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the light");

        return output.view.Get();
    }
    catch (const std::exception& error)
    {
        log(std::string("Global illumination: ") + error.what());
        return nullptr;
    }
}

ID3D11ShaderResourceView* shadowDebug(UINT width, UINT height) noexcept
{
    if (!shadowing.load(std::memory_order_relaxed) || failed || sinceKick() > 1000) return nullptr;

    std::lock_guard lock(lightMutex);
    const bool fits = shadowDebugWritten && shadowDebugOutput.width == width && shadowDebugOutput.height == height;
    return fits ? shadowDebugOutput.view.Get() : nullptr;
}

ID3D11ShaderResourceView* lightDebug(UINT width, UINT height) noexcept
{
    if (!lighting.load(std::memory_order_relaxed) || failed || sinceKick() > 1000) return nullptr;

    std::lock_guard lock(lightMutex);
    const bool fits = lightDebugWritten && lightDebugOutput.width == width && lightDebugOutput.height == height;
    return fits ? lightDebugOutput.view.Get() : nullptr;
}

std::string shadowProbe()
{
    auto text = gi_renderer::shadowProbe();
    const auto lamps = lamp_shadow_rays::probe();
    if (!text.empty() && !lamps.empty()) text += "\n";

    return text + lamps;
}

ID3D11ShaderResourceView* sunShadow(UINT width, UINT height) noexcept
{
    if (!shadowing.load(std::memory_order_relaxed) || failed || sinceKick() > 1000) return nullptr;

    try
    {
        std::lock_guard lock(lightMutex);
        if (!lightDevice) return nullptr;
        if (resizeShared(shadowOutput, lightDevice.Get(), width, height, DXGI_FORMAT_R16_FLOAT, "Creating the shadow")) shadowWritten = false;

        return shadowWritten ? shadowOutput.view.Get() : nullptr;
    }
    catch (const std::exception& error)
    {
        log(std::string("Ray-traced shadows: ") + error.what());
        return nullptr;
    }
}

std::optional<ReconstructionGuides> reconstructionGuides(UINT width, UINT height) noexcept
{
    if (!enabled.load(std::memory_order_relaxed) || failed || sinceKick() > 1000) return std::nullopt;

    std::lock_guard lock(executionMutex);
    if (!guidesKick || guidesKick != kicks || guideNormals.width != width || guideNormals.height != height) return std::nullopt;

    return ReconstructionGuides{guideNormals.texture, guideDiffuse.texture, guideSpecular.texture, guideNormals.access};
}

void reconstructed() noexcept
{
    reconstructions.fetch_add(1, std::memory_order_relaxed);
}

bool isPathTracing() noexcept
{
    return asksPathTracing.load(std::memory_order_relaxed) && wasPathTraced();
}

bool traceSurfaces() noexcept
{
    // Lists recorded before the scene stopped can still run.
    if (!enabled.load(std::memory_order_relaxed)) return false;

    std::lock_guard lock(executionMutex);
    if (!immediate || failed) return false;

    try
    {
        const performance::Timed timed(immediate.Get(), performance::Pass::GiWait);
        const auto inputsReady = fenceValue + 1;
        const auto traced = fenceValue + 2;
        check(immediate->Signal(fence.Get(), inputsReady), "Signalling the glass");
        immediate->Flush();
        fenceValue = inputsReady;
        if (!gi_renderer::traceSurfaces(inputsReady, traced)) return false;

        fenceValue = traced;
        check(immediate->Wait(fence.Get(), traced), "Waiting for the glass's reflections");
        lightSignalled = lightWaited = traced;

        return true;
    }
    catch (const std::exception& error)
    {
        fail(error.what());
        return false;
    }
}

ID3D11ShaderResourceView* reflection(UINT width, UINT height) noexcept
{
    if (!wasPathTraced()) return nullptr;

    std::lock_guard lock(lightMutex);
    return reflectionOutput.width == width && reflectionOutput.height == height ? reflectionOutput.view.Get() : nullptr;
}

ID3D11ShaderResourceView* reflectionMap(UINT width, UINT height) noexcept
{
    if (!wasPathTraced()) return nullptr;

    std::lock_guard lock(lightMutex);
    return reflectionMapOutput.width == width && reflectionMapOutput.height == height ? reflectionMapOutput.view.Get() : nullptr;
}

std::optional<LampLight> lampLight(UINT width, UINT height) noexcept
{
    if (!lastTraces.load(std::memory_order_relaxed).tracesLampLight || !wasPathTraced()) return std::nullopt;

    std::lock_guard lock(lightMutex);
    if (lampDiffuseOutput.width != width || lampDiffuseOutput.height != height) return std::nullopt;

    return LampLight{lampDiffuseOutput.view.Get(), lampSpecularOutput.view.Get()};
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> surfaceDepth(UINT width, UINT height) noexcept
{
    std::lock_guard lock(lightMutex);
    return sizeOf(publishedDepth.Get()) == std::pair{width, height} ? publishedDepth : nullptr;
}

ID3D11UnorderedAccessView* reflectionDistanceAccess(UINT width, UINT height) noexcept
{
    return reflectionDistance(width, height) ? reflectionDistanceOutput.access.Get() : nullptr;
}

ID3D11Texture2D* reflectionDistance(UINT width, UINT height) noexcept
{
    if (!wasPathTraced()) return nullptr;

    std::lock_guard lock(lightMutex);
    return reflectionDistanceOutput.width == width && reflectionDistanceOutput.height == height ? reflectionDistanceOutput.texture.Get() : nullptr;
}

ID3D11ShaderResourceView* forwardShadow(UINT width, UINT height) noexcept
{
    if (!wasPathTraced()) return nullptr;

    std::lock_guard lock(lightMutex);
    return forwardShadowOutput.width == width && forwardShadowOutput.height == height ? forwardShadowOutput.view.Get() : nullptr;
}

bool active() noexcept
{
    return enabled.load(std::memory_order_relaxed);
}

bool lightRuns() noexcept
{
    return lighting.load(std::memory_order_relaxed) && !failed && sinceKick() < 1000;
}

void recorded(std::uint64_t into, std::uint64_t from) noexcept
{
    gi_inputs::recorded(into, from);
    notes.recorded(into, from);
}

void forget(std::uint64_t contextOrList) noexcept
{
    gi_inputs::forget(contextOrList);
    notes.forget(contextOrList);
}

void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    auto noted = notes.take(list).value_or(Notes{});

    std::lock_guard lock(executionMutex);
    if (!immediate || failed || context != nativeImmediate) return;

    gi_inputs::executing(list);
    if (noted.sky) sky = noted.sky;
    if (noted.sun)
    {
        sun = noted.sun;
        sunSincePresent = true;
    }

    if (noted.shadowRange) shadowRange = *noted.shadowRange;
    if (noted.normals) normals = noted.normals;
    if (noted.albedo) albedo = noted.albedo;

    const auto options = latch.frame();

    try
    {
        if (noted.kick)
        {
            LARGE_INTEGER started{}, finished{};
            QueryPerformanceCounter(&started);
            kick(noted.depth.Get(), options);
            QueryPerformanceCounter(&finished);
            kickTicks += finished.QuadPart - started.QuadPart;
            ++kicksSinceLog;
        }
        // The resolve reads the light this frame's kick asked for.
        if (noted.wait && lightSignalled > lightWaited)
        {
            const performance::Timed timed(immediate.Get(), performance::Pass::GiWait);
            check(immediate->Wait(fence.Get(), lightSignalled), "Waiting for the global illumination");
            lightWaited = lightSignalled;
        }
    }
    catch (const std::exception& error)
    {
        fail(error.what());
    }

    // Noted after the kick: a list's velocity is drawn when the list runs, after the kick before it.
    if (noted.velocity)
    {
        objectVelocity = std::move(noted.velocity);
        objectVelocityDrawn = true;
    }
}

void present() noexcept
{
    expiredNotes += notes.present();
    expiredCameras += gi_inputs::present();

    {
        std::lock_guard lock(executionMutex);
        objectVelocityDrawn = false;
        // The next frame's kick lights with this frame's sun, so none when this frame had no sun pass.
        if (!std::exchange(sunSincePresent, false)) sun.reset();
    }

    const auto asked = latch.present().asked;

    const bool on = asked.needsScene() && !failed;
    lighting = on && asked.runsGlobalIllumination;
    shadowing = on && asked.needsSunRays();
    localShadowing = on && asked.needsLampRays();
    asksPathTracing = asked.runsPathTracing;
    if (enabled.exchange(on) && !on)
    {
        // Off gives the GPU memory back.
        std::lock_guard lock(executionMutex);
        gi_renderer::stop();
        gi_textures::reset();
        lightSignalled = lightWaited = 0;
    }

    gi_inputs::setActive(on);
    engine_scene::setActive(on);
    if constexpr (DiagnosticLog)
    {
        if (on) logStatus();
    }
}
}
