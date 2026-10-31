#pragma once

#include "features/ray_traced_lights/light_choice.hpp"
#include "passes/ray_tracing/scene_description.hpp"
#include "passes/ray_tracing/scene_memory.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <d3d11_4.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The global illumination's Direct3D 12 side (docs/DEVELOPMENT.md, "Global illumination"): a device
// on the game's GPU that keeps the ray-traced scene (gi_scene.hpp) from the game's models, characters
// and terrain, and traces it. Direct3D 11 hands it the frame's inputs in shared textures and the
// two take turns through a shared fence. Everything here runs on the game's render thread.
namespace phantom_fx::gi_renderer
{
using light_choice::LocalLight;
using scene_description::CharacterInstance;
using scene_description::Model;
using scene_description::ModelInstance;
using scene_description::ModelPart;
using scene_description::Transform;

// Textures Direct3D 11 made shareable (D3D11_RESOURCE_MISC_SHARED_NTHANDLE).
struct Textures
{
    ID3D11Texture2D* depth{};    // R32_FLOAT, reversed-Z
    ID3D11Texture2D* normal{};   // R10G10B10A2_UNORM, world space
    ID3D11Texture2D* albedo{};   // R8G8B8A8_UNORM, linear
    ID3D11Texture2D* output{};   // R16G16B16A16_FLOAT, what Direct3D 11 reads back
    ID3D11Texture2D* shadow{};   // R16_FLOAT, the sun's ray-traced shadow; null while off
    ID3D11Texture2D* shadowDebug{}; // R8G8B8A8_UNORM, the shadow debug view (DebugShadows); null while off
    // A copy of the game's velocity for its moving meshes, for the denoisers (g_ObjectVelocity in
    // gi_common.hlsli), as the game's view reads it; null when this frame's did not come before the kick.
    ID3D11Texture2D* velocity{};
    DXGI_FORMAT velocityFormat{};
    ID3D11Texture2D* localLightShadow{}; // R16_FLOAT, the share of the point and spot lights' light that reaches; null without lights
    ID3D11Texture2D* lightDebug{}; // R8G8B8A8_UNORM, the light's debug view (CompositeLight, LightDebugView); null while off
    // Path tracing (reflections.hlsl): Ray Reconstruction's guides (ray_tracing::ReconstructionGuides), the reflected light
    // (R16G16B16A16_FLOAT, times the specular albedo and the exposure), its hit distance (R16_FLOAT) and the game's
    // reflection map from its hits (R16G16B16A16_FLOAT); null while off.
    ID3D11Texture2D* guideNormals{};
    ID3D11Texture2D* guideSpecular{};
    ID3D11Texture2D* reflection{};
    ID3D11Texture2D* reflectionDistance{};
    ID3D11Texture2D* reflectionMap{};
    // And its lamps (direct_light.hlsl): the material guides (the specular colour and the translucency; the light
    // through the leaves' floor, F0 and the anisotropy; R8G8B8A8_UNORM), the game's specular table (64 x 256,
    // R8G8B8A8_UNORM), the masks' atlas (null without masks) and the lamps' diffuse and specular light
    // (R16G16B16A16_FLOAT); null while off.
    ID3D11Texture2D* guideMaterial{};
    ID3D11Texture2D* guideLobe{};
    ID3D11Texture2D* specularTable{};
    ID3D11Texture2D* lampMasks{};
    ID3D11Texture2D* lampDiffuse{};
    ID3D11Texture2D* lampSpecular{};
    // And its hits (gi_hit_light.hlsli): each row of the game's material table as its specular colour (256 x 1,
    // R16G16B16A16_FLOAT), and the sky from every side (sky_capture.hpp), null until captured.
    ID3D11Texture2D* materialSpecular{};
    ID3D11Texture2D* skyCube{};
    // And glass and mirrors (glass.hpp), which traceSurfaces traces: the nearest one's depth (R32_FLOAT) and normal with its
    // roughness (R16G16B16A16_FLOAT) at each pixel, what it reflects (R16G16B16A16_FLOAT); null while off.
    ID3D11Texture2D* surfaceDepth{};
    ID3D11Texture2D* surfaceNormal{};
    ID3D11Texture2D* surfaceReflection{};
};

using Matrix = std::array<float, 16>; // Rows applied as dot(row, v), as the engine's

// What the light's debug view shows in textures.lightDebug.
enum class LightDebugView : std::uint32_t
{
    None,
    History = GI_LIGHT_DEBUG_HISTORY,       // The denoiser's history
    Validation = GI_LIGHT_DEBUG_VALIDATION, // NRD's validation layer
};

using SceneTexture = scene_contents::SceneTexture;

// The game's terrain heightmap as its terrain shaders read it (engine_scene::TerrainInputs), shared
// copies of its most detailed level, for the rays' terrain rings (TerrainGrid in gi_scene.hlsl).
struct Terrain
{
    ID3D11Texture2D* pages{};   // The page table
    ID3D11Texture2D* heights{}; // The height atlas
    DXGI_FORMAT pagesFormat{}, heightsFormat{}; // As the game views them
    std::array<float, 4> scale{}, offset{}, uvConverter{}, texelCorrect{};
};

struct Frame
{
    Textures textures;
    std::uint32_t width{}, height{};
    Matrix viewProjection{}, previousViewProjection{};
    Matrix view{}, projection{}, previousView{}, previousProjection{}; // The engine's m_view and m_projection (cPSScene c8, c4)
    std::array<float, 4> jitter{}; // The projection's jitter in NDC (cPSScene c28): xy this frame's, zw the last frame's
    bool cameraCut{}; // The light's denoiser drops its history (camera::cameraCut)
    std::array<float, 3> eye{};
    float nearPlane{}, farPlane{};
    float verticalFov{}; // Radians
    float outputScale{}; // From the light as given (sun, sky) to the game's diffuse light units: the exposure
    // Towards the sun, world space; its diffuse light at normal incidence, before exposure and scaled
    std::array<float, 3> sunDirection{}, sunColor{};
    std::array<std::array<float, 3>, 9> skyRadiance{}; // L2 spherical harmonics, world space, in the sun colour's units
    // The game's loaded models: those first seen since the last frame, and every placement this
    // frame (empty when no newer list came: the last one stays).
    std::vector<std::shared_ptr<const Model>> newModels;
    std::vector<ModelInstance> modelInstances;
    std::vector<CharacterInstance> characters; // With modelInstances
    bool modelInstancesUpdated{};
    std::vector<SceneTexture> newTextures; // Copied, or copied again as the game streamed more of it in, since the last frame
    std::optional<Terrain> terrain; // Absent until the game's terrain draws give its inputs
    std::optional<float> seaLevel;  // The sea's (sea::level); absent in a frame without the add-on's sea
    std::uint32_t frameIndex{};
    bool showScene{}; // The output shows the traced scene instead of the light
    bool logOften{};  // Diagnostics: the light's statistics logged every few frames (as each readback returns), not every ten seconds
    bool traceLight = true; // A cost measurement leaves the light out: the scene is still built
    // DLSS Ray Reconstruction denoises: the light takes a sample a render pixel and skips its denoiser, and
    // the sun's and the lights' shadows go out as their rays found them (GIFrameConstants::reconstruction).
    bool forReconstruction{};
    // With it, path tracing: the reflections and the point and spot lights traced (path_tracing_rays.hpp).
    bool isPathTraced{};
    // Self-lit meshes' light: its scale from the game's units, and the most radiance one lights others with, so scaled.
    float emissionScale{}, emissionCap{};
    // The sun's ray-traced shadow into textures.shadow; the game's shadow map range (cPSLight c8.x,
    // c8.y, c6.x, and 1 once known) and the tangent of the sun's angular radius.
    bool traceShadows{};
    std::array<float, 4> shadowRange{};
    float sunTangent{};
    // Diagnostics, with the shadows: the debug view into textures.shadowDebug, and the probe at the
    // screen's centre (shadowProbe).
    bool debugShadows{};
    bool shadowStarts{};     // The debug view: where the shadow rays leave each pixel (ShadowStartColor)
    bool twoSidedShadows{};  // Diagnostics: rays towards lights blocked by every surface from both sides, as until 2026-10-26
    LightDebugView lightDebugView{}; // Diagnostics, with textures.lightDebug
    std::uint32_t lightRays{2}; // The light's rays a light pixel, 1 to 4 (TraceLight)
    // The point and spot lights whose shadows go into textures.localLightShadow, at most GI_MAX_LOCAL_LIGHTS,
    // and the rays a pixel they take.
    std::vector<LocalLight> localLights;
    std::uint32_t localLightRays{1};
    bool probeLights{}; // Diagnostics: the lamp probe at the screen's centre (lightProbe), sun or none
    std::uint64_t waitValue{}, signalValue{}; // On the shared fence
};

struct Result
{
    // Models let go after a while unplaced, to read again when the game places them again, and the
    // textures no model left uses.
    std::vector<std::uint64_t> releasedModels, releasedTextures;
};

// Throws when the GPU cannot trace rays (DXR 1.1) or Direct3D 12 fails.
void start(ID3D11Device* device, HANDLE sharedFence);
void stop() noexcept;
bool started() noexcept;

// Records and submits the frame: waits for frame.waitValue on the shared fence, signals
// frame.signalValue when the output is written. Throws on Direct3D 12 errors.
Result render(const Frame& frame);

// After the game drew this frame's glass and mirrors: their reflections, on the scene render built. Waits for `waitValue` on
// the shared fence and signals `signalValue` when they are written. False, with nothing submitted, when the last render
// traced no reflections. Throws on Direct3D 12 errors.
bool traceSurfaces(std::uint64_t waitValue, std::uint64_t signalValue);

// For the overlay: instances, triangles and memory in use.
std::string status();
// For the overlay: the latest shadow probe as text, a few frames old; empty while the debug view is
// off. Logged by itself whenever what blocks the probe's sun ray changes.
std::string shadowProbe();
// The same of the lamp probe: the strongest point and spot lights at the screen's centre and what blocks
// each; logged whenever those change.
std::string lightProbe();
}
