#include "features/ray_traced_lights/local_lights.hpp"

#include "camera.hpp"
#include "command_list_notes.hpp"
#include "constant_capture.hpp"
#include "d3d11_helpers.hpp"
#include "features/path_tracing/path_tracing.hpp"
#include "features/ray_traced_lights/lamp_shadow_rays.hpp"
#include "frame_log.hpp"
#include "game_lights.hpp"
#include "game_shaders.hpp"
#include "log.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"
#include "performance.hpp"
#include "resource_dump.hpp"

#include <Windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace phantom_fx::local_lights
{
namespace
{
using Microsoft::WRL::ComPtr;
using namespace game_lights;
using game_shaders::GamePass, game_shaders::GameShader, game_shaders::GameShaders;
using game_constants::LampLightConstants, game_constants::LampMaterialConstants, game_constants::SceneConstants;
using light_choice::Bounds, light_choice::DrawnLight, light_choice::LocalLight;

// Diagnostics: which lights are drawn over all their reach, shadowed by the rays alone, rather than
// within the volume the game bounds each by: none, those the game gives a shadow map, or every one.
enum class BeyondVolumes : std::uint8_t { None, ShadowMapped, All };

// What the light shaders read: t0 to t10 (the G-buffer, its depth at t2, the mask at t4, the specular
// tables at t8 and t9, the material table at t10), s8 to s11, and the scene, light and material constants
// at b2 to b4.
constexpr UINT ResourceSlots = 11, DepthSlot = 2, MaskSlot = 4, FirstSampler = 8, SamplerCount = 4;
// A light's volume as copied from its draw: its indices, then from VolumeVertexStart its vertices. The
// game's had at most 138 indices and 46 vertices of 16 bytes (2026-10-24).
constexpr UINT VolumeBytes = 4096, VolumeVertexStart = 1024;

// The game's lights with specular have a scale of 1; it is 0 for those without, as its flashlights.
constexpr float AddedSpecular = 1.0f;
// 1 / the penumbra (penumbraScale) from which a shadow-mapped spot is a beam the game shines in from
// outside through an opening (isBeam). The cells' moonlight has 100000; every other light in 13
// captures had at most 66 (2026-10-24).
constexpr float BeamEdge = 1000.0f;

// The full-screen triangle in the light passes' vertex shaders' form (Lighting_*_vs): the clip-space
// position twice, then where it samples the light targets, from the viewport within them.
constexpr char FullScreenSource[] = R"(
cbuffer LightPass : register(b0)
{
    float4 ViewToUv; // xy: the viewport's size over the targets'; zw: its corner over the targets'
    uint4 Shadow;    // y: 1 when the light was traced, so the share holds its shadow
};

void main(uint id : SV_VertexID, out float4 position : SV_Position, out float4 clip : TEXCOORD0, out float4 sampling : TEXCOORD1)
{
    float2 xy = float2(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0);
    position = float4(xy, 0.0, 1.0);
    clip = position;
    sampling = float4((xy * float2(0.5, -0.5) + 0.5) * ViewToUv.xy + ViewToUv.zw, 0.0, 1.0);
}
)";

// The light's volume as the game drew it, in the same form, from a copy of its indices and vertices.
constexpr char VolumeSource[] = R"(
cbuffer LightPass : register(b0)
{
    float4 ViewToUv;
    uint4 Shadow;
    uint4 Volume; // x: 1 for 32-bit indices, y: the vertices' stride, z: where they start
};

ByteAddressBuffer Drawn : register(t0);

void main(uint id : SV_VertexID, out float4 position : SV_Position, out float4 clip : TEXCOORD0, out float4 sampling : TEXCOORD1)
{
    uint index = Volume.x ? Drawn.Load(id * 4) : (Drawn.Load((id * 2) & ~3u) >> ((id % 2) * 16)) & 0xFFFF;
    position = asfloat(Drawn.Load4(Volume.z + index * Volume.y));
    clip = position;
    sampling = float4((position.xy * float2(0.5, -0.5) + 0.5 * position.w) * ViewToUv.xy + ViewToUv.zw * position.w, position.zw);
}
)";

// On the light's far faces, the light's shadow into both light targets' alpha, for the light's blend to
// scale it by; where the surface lies past them, none of its light. The full-screen triangle, at depth 0
// (as far as can be), lies past every surface.
constexpr char ShadowSource[] = R"(
cbuffer LightPass : register(b0)
{
    float4 ViewToUv;
    uint4 Shadow;
};

Texture2D<float> Share : register(t0); // The share of the traced lights' light that reaches
Texture2D<float> Depth : register(t1); // Reversed: the nearer, the greater

void main(float4 position : SV_Position, out float4 diffuse : SV_Target0, out float4 specular : SV_Target1)
{
    int3 pixel = int3(position.xy, 0);
    float lit = Shadow.y ? Share.Load(pixel) : 1.0;
    diffuse = specular = float4(0.0, 0.0, 0.0, position.z <= Depth.Load(pixel) ? lit : 0.0);
}
)";

// On the light's near faces: a surface short of them is outside its volume, so none of its light.
constexpr char BeforeVolumeSource[] = R"(
Texture2D<float> Depth : register(t1);

void main(float4 position : SV_Position, out float4 diffuse : SV_Target0, out float4 specular : SV_Target1)
{
    if (position.z >= Depth.Load(int3(position.xy, 0))) discard;
    diffuse = specular = 0.0;
}
)";

// A light's volume's bounds in the world, from the copy of its indices and vertices (clip space, as the
// game drew it) through the inverse of the light pass's m_projectionView.
constexpr char BoundsSource[] = R"(
cbuffer Bounds : register(b0)
{
    float4 ClipToWorld[4]; // Rows applied as dot(row, v)
    uint4 Volume;          // x: 1 for 32-bit indices, y: the vertices' stride, z: where they start, w: how many indices
    uint4 Slot;            // x: the light's place in Bounds
};

ByteAddressBuffer Drawn : register(t0);
RWStructuredBuffer<float4> Bounds : register(u0); // Two a light: lowest, highest

[numthreads(1, 1, 1)]
void main()
{
    float3 lowest = 1e30, highest = -1e30;
    for (uint i = 0; i < Volume.w; ++i)
    {
        uint index = Volume.x ? Drawn.Load(i * 4) : (Drawn.Load((i * 2) & ~3u) >> ((i % 2) * 16)) & 0xFFFF;
        float4 clip = asfloat(Drawn.Load4(Volume.z + index * Volume.y));
        float4 world = float4(dot(ClipToWorld[0], clip), dot(ClipToWorld[1], clip), dot(ClipToWorld[2], clip), dot(ClipToWorld[3], clip));
        float3 corner = world.xyz / world.w;
        lowest = min(lowest, corner);
        highest = max(highest, corner);
    }

    Bounds[Slot.x * 2] = float4(lowest, 1.0);
    Bounds[Slot.x * 2 + 1] = float4(highest, 1.0);
}
)";

struct PassConstants
{
    float viewToUv[4];
    std::uint32_t shadow[4];
    std::uint32_t volume[4];
};

struct BoundsConstants
{
    float clipToWorld[4][4];
    std::uint32_t volume[4];
    std::uint32_t slot[4];
};

// A light's volume's bounds in the world are worked out as it is drawn and read back this many frames
// later, by light (keyOf): the global illumination's bounce and the rays' picks keep it within them, as
// the game does. The game's lights hold still, so the last frame's bounds are this frame's.
constexpr std::uint32_t BoundsReadbacks = 3;

// A light's volume as its draws had it bound (a box, for a spot its cone within one), copied: the game
// lights only within it (docs/DEVELOPMENT.md, "Ray-traced lights", "Within the game's volume").
struct Volume
{
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> view; // Raw
    UINT count{};                          // Indices
    UINT stride{};
    bool wideIndices{};                    // 32-bit
};

// A light's lighting draw as recorded, with what drawing it again on the immediate context needs.
struct Light
{
    DrawnLight drawn;
    std::array<ComPtr<ID3D11ShaderResourceView>, ResourceSlots> resources;
    std::array<ComPtr<ID3D11SamplerState>, SamplerCount> samplers;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> targets;
    Volume volume; // None when it could not be copied: drawn over all its reach
    bool traced{}; // Given to the rays at the frame's kick: the share holds its shadow
};

// What a deferred context, then its command list, recorded: its lights, and whether it has a draw point
// (noteDrawPoint).
struct Notes
{
    std::vector<Light> lights;
    bool drawPoint{};

    void merge(Notes later)
    {
        lights.insert(lights.end(), std::make_move_iterator(later.lights.begin()), std::make_move_iterator(later.lights.end()));
        drawPoint = drawPoint || later.drawPoint;
    }
};

// What the effect plan asked for (configure): the ray-traced lights, or path tracing alone, which takes the
// game's lights only while it runs (ray_tracing::isPathTracing).
std::atomic<bool> wantedRunning{}, isForPathTracingOnly{};
// On with the pipeline and a twin of the game's: the draw points and the lists are followed. Active once a
// frame also had a draw point, whose list draws them again: the game's lights are taken over. Menus and
// loading screens, without one, keep the game's.
std::atomic<bool> enabled{}, active{};
// Multiplies the game's flashlights' light; their reach grows with its square root, as a brighter light
// falls to the same dimness further away.
std::atomic<float> flashlightBrightness{1};
std::atomic<BeyondVolumes> beyondVolumes{};

// The game's shaders that draw the lights of each kind again: their twins (game_lights::isTwin).
constexpr auto TwinRows = [] {
    std::array<const GameShader*, static_cast<std::size_t>(Twin::Count)> rows{};
    for (const auto& row : GameShaders)
        if (row.light && isTwin(*row.light)) rows[static_cast<std::size_t>(row.light->twin)] = &row;

    return rows;
}();

CommandListNotes<Notes> notes;

// The volumes' buffers of lights drawn again, for lights recorded after.
std::mutex volumeMutex;
std::vector<Volume> spareVolumes;

// ---- The immediate context's side, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ID3D11DeviceContext* immediate{};
ComPtr<ID3D11VertexShader> fullScreenShader, volumeShader;
ComPtr<ID3D11PixelShader> shadowShader, beforeVolumeShader;
ComPtr<ID3D11BlendState> alphaOnly, shadowed;
ComPtr<ID3D11RasterizerState> scissored, farFaces, nearFaces;
ComPtr<ID3D11DepthStencilState> noDepth;
ComPtr<ID3D11Buffer> passBuffer, sceneBuffer, lightBuffer, materialBuffer;
bool pipelineFailed{};
// The frame's lights from the lists run so far, and whether a list with a draw point has run.
std::vector<Light> frameLights;
bool drawPointRan{};
// The volumes' bounds: the shader and its output, its copies on their way back, the lights each holds by
// slot, and this frame's so far.
ComPtr<ID3D11ComputeShader> boundsShader;
ComPtr<ID3D11Buffer> boundsConstants, boundsBuffer;
ComPtr<ID3D11UnorderedAccessView> boundsAccess;
std::array<ComPtr<ID3D11Buffer>, BoundsReadbacks> boundsReadbacks;
std::array<std::vector<std::uint64_t>, BoundsReadbacks> readbackLights;
std::vector<std::uint64_t> frameBoundsLights;
std::uint32_t boundsFrame{};

// The bounds read back, by light; taken at the kick. Kept until detach: the ten-second log counts them,
// as a light whose key changes every frame would add one each frame.
std::mutex boundsMutex;
std::unordered_map<std::uint64_t, Bounds> volumeBounds;

// Diagnostics, logged every ten seconds: lights drawn with their shadow and without it (recorded after
// the kick, or the shadow not traced), those of them drawn without their volume, left to the next frame's
// camera or over the mask's bits at the kick, and lost (a frame ended without a draw point after them).
std::atomic<std::uint64_t> drawnShadowed{}, drawnUnshadowed{}, drawnWithoutVolume{}, otherFrame{}, overCap{}, lost{};
// And those path tracing lit (drawPathTracedLights).
std::atomic<std::uint64_t> drawnPathTraced{};
// Diagnostics: contexts' or lists' notes never handed over (CommandListNotes::present).
std::atomic<std::uint64_t> expiredNotes{};

// The shaders, states and buffers, once; call with resourceMutex held. False when unavailable.
bool ensurePipeline()
{
    if (fullScreenShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        shadowShader = pixelShader(device.Get(), ShadowSource, "light-shadow");
        beforeVolumeShader = pixelShader(device.Get(), BeforeVolumeSource, "light-before-volume");
        const auto volumeCode = compile(VolumeSource, "light-volume", "vs_5_0");
        check(device->CreateVertexShader(volumeCode->GetBufferPointer(), volumeCode->GetBufferSize(), nullptr, volumeShader.ReleaseAndGetAddressOf()),
            "Creating the lights' volume vertex shader");

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        check(device->CreateBlendState(&blend, alphaOnly.ReleaseAndGetAddressOf()), "Creating the lights' shadow blend");

        // The game's (one, one) for its lights, scaled by the shadow in the targets' alpha.
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_ALPHA;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device->CreateBlendState(&blend, shadowed.ReleaseAndGetAddressOf()), "Creating the lights' blend");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        rasterizer.ScissorEnable = TRUE;
        check(device->CreateRasterizerState(&rasterizer, scissored.ReleaseAndGetAddressOf()), "Creating the lights' rasterizer state");

        // The game's volumes face out clockwise, as Direct3D's front faces do (all 241 faces in front of
        // the eye in a capture, 2026-10-24): their far faces are the back faces.
        rasterizer.CullMode = D3D11_CULL_FRONT;
        check(device->CreateRasterizerState(&rasterizer, farFaces.ReleaseAndGetAddressOf()), "Creating the lights' far faces state");
        rasterizer.CullMode = D3D11_CULL_BACK;
        check(device->CreateRasterizerState(&rasterizer, nearFaces.ReleaseAndGetAddressOf()), "Creating the lights' near faces state");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        check(device->CreateDepthStencilState(&depth, noDepth.ReleaseAndGetAddressOf()), "Creating the lights' depth state");

        passBuffer = makeConstantBuffer(device.Get(), sizeof(PassConstants), "Creating the lights' pass constants");
        sceneBuffer = makeConstantBuffer(device.Get(), sizeof(SceneConstants), "Creating the lights' scene constants");
        lightBuffer = makeConstantBuffer(device.Get(), sizeof(LampLightConstants), "Creating the lights' light constants");
        materialBuffer = makeConstantBuffer(device.Get(), sizeof(LampMaterialConstants), "Creating the lights' material constants");

        boundsShader = computeShader(device.Get(), BoundsSource, "light-volume-bounds");
        boundsConstants = makeConstantBuffer(device.Get(), sizeof(BoundsConstants), "Creating the lights' bounds constants");
        D3D11_BUFFER_DESC bounds{};
        bounds.ByteWidth = GI_MAX_LOCAL_LIGHTS * 2 * 16;
        bounds.Usage = D3D11_USAGE_DEFAULT;
        bounds.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bounds.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bounds.StructureByteStride = 16;
        check(device->CreateBuffer(&bounds, nullptr, boundsBuffer.ReleaseAndGetAddressOf()), "Creating the lights' bounds");
        check(device->CreateUnorderedAccessView(boundsBuffer.Get(), nullptr, boundsAccess.ReleaseAndGetAddressOf()), "Viewing the lights' bounds");
        bounds.Usage = D3D11_USAGE_STAGING;
        bounds.BindFlags = 0;
        bounds.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& readback : boundsReadbacks)
            check(device->CreateBuffer(&bounds, nullptr, readback.ReleaseAndGetAddressOf()), "Creating the lights' bounds readback");

        // Last, so it marks a complete pipeline.
        const auto fullScreenCode = compile(FullScreenSource, "light-full-screen", "vs_5_0");
        check(device->CreateVertexShader(fullScreenCode->GetBufferPointer(), fullScreenCode->GetBufferSize(), nullptr,
                  fullScreenShader.ReleaseAndGetAddressOf()),
            "Creating the lights' vertex shader");
        log("Ray-traced lights ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("Ray-traced lights unavailable: ") + error.what());

        return false;
    }
}

// The twin that draws the lights of `kind` again, once the game has made it.
ID3D11PixelShader* twinOf(Twin kind)
{
    return game_shaders::firstCreated(*TwinRows[static_cast<std::size_t>(kind)]);
}

bool hasAnyTwin()
{
    return std::ranges::any_of(TwinRows, [](const GameShader* row) { return game_shaders::firstCreated(*row) != nullptr; });
}

// The game's light a shader draws, when its twin is there to draw it again.
const GameLight* gameLightOf(const GameShader* shader)
{
    if (!shader || shader->pass != GamePass::LocalLight || !twinOf(shader->light->twin)) return nullptr;

    return &*shader->light;
}

// What the light's lighting draw binds, or nothing when its constants are unknown.
std::optional<Light> noteOf(ID3D11DeviceContext* context, const GameLight* game)
{
    std::array<ID3D11Buffer*, 3> constants{}; // b2 to b4
    context->PSGetConstantBuffers(SceneConstants::Slot, static_cast<UINT>(constants.size()), constants.data());
    const auto scene = constant_capture::written<SceneConstants>(constants[0]);
    const auto light = constant_capture::written<LampLightConstants>(constants[1]);
    const auto material = constant_capture::written<LampMaterialConstants>(constants[2]);
    for (auto* buffer : constants)
        if (buffer) buffer->Release();
    if (!scene || !light || !material) return std::nullopt;

    Light noted{.drawn = {.game = game, .scene = *scene, .light = *light, .material = *material}};
    std::array<ID3D11ShaderResourceView*, ResourceSlots> resources{};
    context->PSGetShaderResources(0, ResourceSlots, resources.data());
    for (UINT i = 0; i < ResourceSlots; ++i) noted.resources[i].Attach(resources[i]);

    std::array<ID3D11SamplerState*, SamplerCount> samplers{};
    context->PSGetSamplers(FirstSampler, SamplerCount, samplers.data());
    for (UINT i = 0; i < SamplerCount; ++i) noted.samplers[i].Attach(samplers[i]);

    std::array<ID3D11RenderTargetView*, 2> targets{};
    context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), nullptr);
    for (std::size_t i = 0; i < targets.size(); ++i) noted.targets[i].Attach(targets[i]);

    UINT viewports = 1;
    context->RSGetViewports(&viewports, &noted.drawn.viewport);
    if (!viewports || !noted.targets[0] || !noted.targets[1]) return std::nullopt;

    // A brighter flashlight: its colour times the brightness, and its falloff's d^16 factor over the
    // brightness^8, which puts the end of its reach (factor^(-1/16)) the brightness's square root further.
    if (game->flashlight)
    {
        const float brightness = flashlightBrightness.load(std::memory_order_relaxed);
        for (auto& channel : noted.drawn.light.color) channel *= brightness;
        noted.drawn.light.falloff[2] /= std::pow(brightness, 8.0f);
    }

    return noted;
}

// A beam the game shines in through an opening, as moonlight through a cell's vent: a spot with a shadow
// map and a hard-edged cone. Its light stands metres outside, past geometry its shadow map leaves out
// and the rays do not, so they found it blocked even through the vent; and, 20 to 40 times as bright as
// the room's lamps, it took their share of the rays where it fell. The game keeps it, with its shadow
// map's pattern (docs/DEVELOPMENT.md, "Ray-traced lights", "Beams").
bool isBeam(ID3D11DeviceContext* context, const GameLight& game)
{
    if (!game.shadowMap || game.shape != Shape::Spot) return false;

    const auto light = constant_capture::pixelShaderReads<LampLightConstants>(context);

    return light && light->penumbraScale >= BeamEdge;
}

// A buffer for a light's volume: a drawn light's, or a new one.
Volume spareVolume(ID3D11Device* creator)
{
    {
        std::lock_guard lock(volumeMutex);
        if (!spareVolumes.empty())
        {
            auto volume = std::move(spareVolumes.back());
            spareVolumes.pop_back();
            return volume;
        }
    }

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = VolumeBytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    Volume volume;
    check(creator->CreateBuffer(&desc, nullptr, volume.buffer.GetAddressOf()), "Creating a light's volume");

    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    view.BufferEx.NumElements = VolumeBytes / 4;
    view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    check(creator->CreateShaderResourceView(volume.buffer.Get(), &view, volume.view.GetAddressOf()), "Viewing a light's volume");

    return volume;
}

// The volume a light's draw has bound, copied on its list before the list's later draws write over it.
// None when it does not fit.
Volume volumeOf(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    try
    {
        ComPtr<ID3D11Buffer> indexBuffer, vertexBuffer;
        DXGI_FORMAT indexFormat{};
        UINT indexOffset{}, stride{}, vertexOffset{};
        context->IAGetIndexBuffer(indexBuffer.GetAddressOf(), &indexFormat, &indexOffset);
        context->IAGetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &vertexOffset);
        if (!indexBuffer || !vertexBuffer || stride < 16 || stride % 4) return {};

        D3D11_BUFFER_DESC indexDesc{}, vertexDesc{};
        indexBuffer->GetDesc(&indexDesc);
        vertexBuffer->GetDesc(&vertexDesc);
        const bool wide = indexFormat == DXGI_FORMAT_R32_UINT;
        const UINT indexSize = wide ? 4 : 2;
        const UINT indexStart = indexOffset + call.first * indexSize, indexBytes = call.count * indexSize;
        const std::int64_t vertexStart = std::int64_t{vertexOffset} + std::int64_t{call.vertexOffset} * stride;
        if (indexBytes > VolumeVertexStart || indexStart + indexBytes > indexDesc.ByteWidth || vertexStart < 0 || vertexStart >= vertexDesc.ByteWidth)
            return {};

        ComPtr<ID3D11Device> creator;
        context->GetDevice(creator.GetAddressOf());
        auto volume = spareVolume(creator.Get());
        const D3D11_BOX indices{indexStart, 0, 0, indexStart + indexBytes, 1, 1};
        context->CopySubresourceRegion(volume.buffer.Get(), 0, 0, 0, 0, indexBuffer.Get(), 0, &indices);

        const auto first = static_cast<UINT>(vertexStart);
        const D3D11_BOX vertices{first, 0, 0, first + std::min(VolumeBytes - VolumeVertexStart, vertexDesc.ByteWidth - first), 1, 1};
        context->CopySubresourceRegion(volume.buffer.Get(), 0, VolumeVertexStart, 0, 0, vertexBuffer.Get(), 0, &vertices);
        volume.count = call.count;
        volume.stride = stride;
        volume.wideIndices = wide;

        return volume;
    }
    catch (...)
    {
        return {};
    }
}

// The bounds of the light's volume as read back, if they have come back yet. A flashlight's moves with it.
std::optional<Bounds> volumeBoundsOf(const DrawnLight& light)
{
    if (light.game->flashlight) return std::nullopt;

    std::lock_guard lock(boundsMutex);
    const auto found = volumeBounds.find(light_choice::keyOf(light));
    if (found == volumeBounds.end()) return std::nullopt;

    return found->second;
}

// The light once more, within the game's volume for it: on the volume's far faces its shadow into the
// targets' alpha, none of it where a surface lies short of the near faces, then on the far faces the
// game's light of its kind with specular scaled by it. Without its volume, a full-screen triangle over
// its reach. Call with resourceMutex held, the pipeline made.
void drawAgain(const Light& light)
{
    const auto& drawn = light.drawn;
    const auto reach = light_choice::reachOf(drawn);
    if (!reach) return;

    const auto [width, height] = sizeOf(light.targets[0].Get());
    if (!width || !height) return;

    ID3D11ShaderResourceView* share = light.traced ? lamp_shadow_rays::share(width, height) : nullptr;
    ++(share ? drawnShadowed : drawnUnshadowed);

    const auto& volume = light.volume;
    if (!volume.view) ++drawnWithoutVolume;

    const auto beyond = beyondVolumes.load(std::memory_order_relaxed);
    const bool withinVolume = volume.view && beyond != BeyondVolumes::All && !(beyond == BeyondVolumes::ShadowMapped && drawn.game->shadowMap);
    const auto& viewport = drawn.viewport;
    const PassConstants pass{{viewport.Width / static_cast<float>(width), viewport.Height / static_cast<float>(height),
                                 viewport.TopLeftX / static_cast<float>(width), viewport.TopLeftY / static_cast<float>(height)},
        {0, share ? 1u : 0u, 0, 0}, {volume.wideIndices ? 1u : 0u, volume.stride, VolumeVertexStart, 0}};
    auto constants = drawn.light;
    if (!drawn.game->specular) constants.specularScale = AddedSpecular;
    check(write(immediate, passBuffer.Get(), pass), "Writing the lights' constants");
    check(write(immediate, sceneBuffer.Get(), drawn.scene), "Writing the lights' constants");
    check(write(immediate, lightBuffer.Get(), constants), "Writing the lights' constants");
    check(write(immediate, materialBuffer.Get(), drawn.material), "Writing the lights' constants");

    ID3D11RenderTargetView* targets[]{light.targets[0].Get(), light.targets[1].Get()};
    immediate->OMSetRenderTargets(2, targets, nullptr);
    immediate->RSSetViewports(1, &viewport);
    immediate->RSSetScissorRects(1, &*reach);

    ID3D11ShaderResourceView* shadowResources[]{share, light.resources[DepthSlot].Get()};
    immediate->PSSetShader(shadowShader.Get(), nullptr, 0);
    immediate->PSSetConstantBuffers(0, 1, passBuffer.GetAddressOf());
    immediate->PSSetShaderResources(0, 2, shadowResources);
    immediate->OMSetBlendState(alphaOnly.Get(), nullptr, 0xFFFFFFFF);
    if (withinVolume)
    {
        immediate->VSSetShader(volumeShader.Get(), nullptr, 0);
        immediate->VSSetShaderResources(0, 1, volume.view.GetAddressOf());
        immediate->RSSetState(farFaces.Get());
        immediate->Draw(volume.count, 0);

        immediate->PSSetShader(beforeVolumeShader.Get(), nullptr, 0);
        immediate->RSSetState(nearFaces.Get());
        immediate->Draw(volume.count, 0);
        immediate->RSSetState(farFaces.Get());
    }
    else
    {
        immediate->VSSetShader(fullScreenShader.Get(), nullptr, 0);
        immediate->RSSetState(scissored.Get());
        immediate->Draw(3, 0);
    }

    std::array<ID3D11ShaderResourceView*, ResourceSlots> resources{};
    for (UINT i = 0; i < ResourceSlots; ++i) resources[i] = light.resources[i].Get();
    std::array<ID3D11SamplerState*, SamplerCount> samplers{};
    for (UINT i = 0; i < SamplerCount; ++i) samplers[i] = light.samplers[i].Get();
    ID3D11Buffer* lightConstants[]{sceneBuffer.Get(), lightBuffer.Get(), materialBuffer.Get()}; // b2 to b4
    immediate->PSSetShader(twinOf(drawn.game->twin), nullptr, 0);
    immediate->PSSetShaderResources(0, ResourceSlots, resources.data());
    immediate->PSSetSamplers(FirstSampler, SamplerCount, samplers.data());
    immediate->PSSetConstantBuffers(SceneConstants::Slot, 3, lightConstants);
    immediate->OMSetBlendState(shadowed.Get(), nullptr, 0xFFFFFFFF);
    immediate->Draw(withinVolume ? volume.count : 3, 0);
}

// The world-space bounds of the frame's lights' volumes into boundsBuffer, a slot each, for present to read
// back into volumeBounds. Call with resourceMutex held, the pipeline made.
void measureVolumes()
{
    bool bound{};
    for (const auto& light : frameLights)
    {
        const auto& volume = light.volume;
        if (!volume.view || light.drawn.game->flashlight || frameBoundsLights.size() >= GI_MAX_LOCAL_LIGHTS) continue;

        camera::Matrix projectionView{};
        for (std::size_t i = 0; i < projectionView.size(); ++i) projectionView[i] = light.drawn.scene.projectionView[i / 4][i % 4];
        const auto clipToWorld = camera::inverse(projectionView);
        BoundsConstants constants{{}, {volume.wideIndices ? 1u : 0u, volume.stride, VolumeVertexStart, volume.count},
            {static_cast<std::uint32_t>(frameBoundsLights.size()), 0, 0, 0}};
        for (std::size_t i = 0; i < clipToWorld.size(); ++i) constants.clipToWorld[i / 4][i % 4] = static_cast<float>(clipToWorld[i]);
        check(write(immediate, boundsConstants.Get(), constants), "Writing the lights' bounds constants");
        if (!bound)
        {
            immediate->CSSetShader(boundsShader.Get(), nullptr, 0);
            immediate->CSSetConstantBuffers(0, 1, boundsConstants.GetAddressOf());
            immediate->CSSetUnorderedAccessViews(0, 1, boundsAccess.GetAddressOf(), nullptr);
            bound = true;
        }

        immediate->CSSetShaderResources(0, 1, volume.view.GetAddressOf());
        immediate->Dispatch(1, 1, 1);
        frameBoundsLights.push_back(light_choice::keyOf(light.drawn));
    }

    if (!bound) return;

    ID3D11ShaderResourceView* noView{};
    ID3D11UnorderedAccessView* noAccess{};
    ID3D11Buffer* noConstants{};
    immediate->CSSetShaderResources(0, 1, &noView);
    immediate->CSSetUnorderedAccessViews(0, 1, &noAccess, nullptr);
    immediate->CSSetConstantBuffers(0, 1, &noConstants);
    immediate->CSSetShader(nullptr, nullptr, 0);
}

// The bounds measured this frame on their way back, and those of a frame BoundsReadbacks - 1 ago into
// volumeBounds once the GPU is past them. Call with resourceMutex held.
void readVolumeBounds()
{
    if (!boundsBuffer) return;

    const auto slot = boundsFrame++ % BoundsReadbacks;
    if (!frameBoundsLights.empty())
    {
        immediate->CopyResource(boundsReadbacks[slot].Get(), boundsBuffer.Get());
        readbackLights[slot] = std::move(frameBoundsLights);
    }
    else readbackLights[slot].clear();
    frameBoundsLights.clear();

    const auto oldest = (slot + 1) % BoundsReadbacks;
    auto& lights = readbackLights[oldest];
    if (lights.empty()) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(immediate->Map(boundsReadbacks[oldest].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) return;

    const auto* corners = static_cast<const float*>(mapped.pData);
    {
        std::lock_guard lock(boundsMutex);
        for (std::size_t i = 0; i < lights.size(); ++i)
            volumeBounds[lights[i]] = {corners[i * 8], corners[i * 8 + 1], corners[i * 8 + 2], corners[i * 8 + 4], corners[i * 8 + 5], corners[i * 8 + 6]};
    }

    immediate->Unmap(boundsReadbacks[oldest].Get(), 0);
    lights.clear();
}

// The checksum of the game's shader for `game`, which names the light in the frame record.
const char* checksumOf(const GameLight* game)
{
    const auto row = std::ranges::find_if(GameShaders, [&](const GameShader& shader) { return shader.light && &*shader.light == game; });

    return row != std::end(GameShaders) ? row->checksum : "unknown";
}

// Developer: while F10 records a frame, what the lights are drawn from and into, in the frame's dump
// folder: the light targets ("when" before or after), and before, the rays' share, the depth and each
// light in world space.
void saveForRecord(const std::filesystem::path& folder, int batch, const char* when)
{
    const auto& first = frameLights.front();
    const auto prefix = "lights-" + std::to_string(batch) + "-";
    const auto save = [&](ID3D11View* view, DXGI_FORMAT format, const std::string& name) {
        if (!view) return;

        ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.GetAddressOf());
        saveTexture(immediate, resource.Get(), format, folder / (prefix + name + ".dds"));
    };

    save(first.targets[0].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, std::string(when) + "-diffuse");
    save(first.targets[1].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, std::string(when) + "-specular");
    if (std::string(when) != "before") return;

    const auto [width, height] = sizeOf(first.targets[0].Get());
    save(lamp_shadow_rays::share(width, height), DXGI_FORMAT_R16_FLOAT, "share");
    save(first.resources[DepthSlot].Get(), DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, "depth");

    std::ofstream list(folder / (prefix + "lights.txt"), std::ios::trunc);
    for (const auto& light : frameLights)
    {
        const auto& drawn = light.drawn;
        const auto bounds = volumeBoundsOf(drawn);
        const auto rays = light_choice::forRays(drawn, bounds);
        const auto at = game_constants::toWorld(drawn.scene, drawn.light.position, 1);
        const auto way = game_constants::toWorld(drawn.scene, drawn.light.axis, 0);
        list << checksumOf(drawn.game) << (light.traced ? " traced" : " untraced") << (light.volume.view ? " volume " : " no volume ")
             << light.volume.count << " at " << at[0] << ' ' << at[1] << ' ' << at[2] << " way " << way[0] << ' ' << way[1] << ' ' << way[2]
             << " colour " << drawn.light.color[0] << ' ' << drawn.light.color[1] << ' ' << drawn.light.color[2] << " range " << rays.range
             << " shadow map from " << rays.shadowStart;
        if (bounds)
            list << " volume from " << (*bounds)[0] << ' ' << (*bounds)[1] << ' ' << (*bounds)[2] << " to " << (*bounds)[3] << ' ' << (*bounds)[4] << ' '
                 << (*bounds)[5];
        list << " eye " << drawn.scene.eye[0] << ' ' << drawn.scene.eye[1] << ' ' << drawn.scene.eye[2] << '\n';
    }
}

// Path tracing's light (path_tracing::addLampLight) once for every light the frame's kick traced (`traced`);
// false while it does not light them.
bool addPathTracedLights()
{
    const auto traced = std::ranges::find_if(frameLights, &Light::traced);
    if (traced == frameLights.end() || !path_tracing::addLampLight(immediate, traced->targets[0].Get(), traced->targets[1].Get())) return false;

    drawnPathTraced += std::ranges::count_if(frameLights, &Light::traced);

    return true;
}

// The frame's lights so far, on the immediate context; call with resourceMutex held. The command lists
// the immediate context runs next set their own state, so this only unbinds what it bound.
void drawFrameLights()
{
    if (frameLights.empty()) return;

    try
    {
        if (!ensurePipeline() || !immediate)
        {
            lost += frameLights.size();
            frameLights.clear();
            return;
        }

        // Path tracing lit the lights the kick traced, timed as its own; the others are drawn again, unshadowed.
        const bool isPathTraced = addPathTracedLights();
        const performance::Timed timed(immediate, performance::Pass::LocalLights);
        immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        immediate->IASetInputLayout(nullptr);
        immediate->VSSetConstantBuffers(0, 1, passBuffer.GetAddressOf());
        immediate->GSSetShader(nullptr, nullptr, 0);
        immediate->OMSetDepthStencilState(noDepth.Get(), 0);
        static int recordBatch{};
        const auto record = DiagnosticLog ? frame_log::recordingDirectory() : std::filesystem::path{};
        if (record.empty()) recordBatch = 0;
        else saveForRecord(record, recordBatch, "before");

        for (const auto& light : frameLights)
            if (!isPathTraced || !light.traced) drawAgain(light);

        measureVolumes();
        if (!record.empty()) saveForRecord(record, recordBatch++, "after");

        // The draws reading the volumes are queued: a list that copies into one again runs after them.
        std::lock_guard lock(volumeMutex);
        for (auto& light : frameLights)
            if (light.volume.buffer) spareVolumes.push_back(std::move(light.volume));
    }
    catch (const std::exception& error)
    {
        log(std::string("Ray-traced lights: ") + error.what());
    }

    frameLights.clear();
    std::array<ID3D11ShaderResourceView*, ResourceSlots> noResources{};
    std::array<ID3D11Buffer*, 5> noConstants{};
    immediate->PSSetShaderResources(0, ResourceSlots, noResources.data());
    immediate->VSSetShaderResources(0, 1, noResources.data());
    immediate->PSSetConstantBuffers(0, static_cast<UINT>(noConstants.size()), noConstants.data());
    immediate->VSSetConstantBuffers(0, 1, noConstants.data());
    immediate->OMSetRenderTargets(0, nullptr, nullptr);
    immediate->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    immediate->OMSetDepthStencilState(nullptr, 0);
    immediate->RSSetState(nullptr);
    immediate->VSSetShader(nullptr, nullptr, 0);
    immediate->PSSetShader(nullptr, nullptr, 0);
}

// Diagnostics: what became of the lights, every ten seconds while any came.
void logLights()
{
    static std::uint64_t lastLog{};
    const auto now = GetTickCount64();
    if (now - lastLog < 10'000) return;

    lastLog = now;
    const auto shadowedCount = drawnShadowed.exchange(0), unshadowedCount = drawnUnshadowed.exchange(0);
    const auto withoutVolumeCount = drawnWithoutVolume.exchange(0);
    const auto otherCount = otherFrame.exchange(0), overCount = overCap.exchange(0), lostCount = lost.exchange(0);
    const auto expiredCount = expiredNotes.exchange(0);
    const auto pathTracedCount = drawnPathTraced.exchange(0);
    if (shadowedCount + unshadowedCount + otherCount + overCount + lostCount + expiredCount + pathTracedCount == 0) return;

    std::size_t boundsCount{};
    {
        std::lock_guard lock(boundsMutex);
        boundsCount = volumeBounds.size();
    }

    log("Lights in ten seconds: " + std::to_string(pathTracedCount) + " path traced, " + std::to_string(shadowedCount) + " drawn with their ray-traced shadow, "
        + std::to_string(unshadowedCount)
        + " without (recorded after the kick, or not traced), " + std::to_string(withoutVolumeCount)
        + " of all drawn without the game's volume; at the kicks " + std::to_string(otherCount) + " left for another frame's camera, "
        + std::to_string(overCount) + " over the " + std::to_string(GI_MAX_LOCAL_LIGHTS) + " the rays take; " + std::to_string(lostCount)
        + " lost (no draw point after them); " + std::to_string(expiredCount) + " contexts' or lists' notes expired; "
        + std::to_string(boundsCount) + " lights' volume bounds kept.");
}
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* gameImmediate) noexcept
{
    const bool enable = wantedRunning;
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    immediate = gameImmediate;
    if (enable) ensurePipeline();
}

void detach() noexcept
{
    enabled = active = false;
    {
        std::lock_guard lock(resourceMutex);
        fullScreenShader.Reset();
        volumeShader.Reset();
        shadowShader.Reset();
        beforeVolumeShader.Reset();
        alphaOnly.Reset();
        shadowed.Reset();
        scissored.Reset();
        farFaces.Reset();
        nearFaces.Reset();
        noDepth.Reset();
        passBuffer.Reset();
        sceneBuffer.Reset();
        lightBuffer.Reset();
        materialBuffer.Reset();
        boundsShader.Reset();
        boundsConstants.Reset();
        boundsBuffer.Reset();
        boundsAccess.Reset();
        for (auto& readback : boundsReadbacks) readback.Reset();
        for (auto& lights : readbackLights) lights.clear();
        frameBoundsLights.clear();
        frameLights.clear();
        drawPointRan = false;
        device.Reset();
        immediate = nullptr;
        pipelineFailed = false;
    }

    notes.clear();

    {
        std::lock_guard lock(volumeMutex);
        spareVolumes.clear();
    }

    {
        std::lock_guard lock(boundsMutex);
        volumeBounds.clear();
    }
}

void configure(const Configuration& configuration) noexcept
{
    // Path tracing lights them from their records (path_tracing::addLampLight), with or without the ray-traced lights.
    const bool rayTraced = configuration.effects.runs(Effect::RayTracedLights);
    const bool running = rayTraced || configuration.effects.runs(Effect::PathTracing);
    wantedRunning = running;
    isForPathTracingOnly = !rayTraced;
    flashlightBrightness = configuration.settings.rayTracedLights.flashlightBrightness;
    beyondVolumes = static_cast<BeyondVolumes>(std::clamp(configuration.diagnostics.lightVolumes, 0, 2));

    if (!running) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

Health health()
{
    if (!wantedRunning) return {};

    {
        std::lock_guard lock(resourceMutex);
        if (pipelineFailed) return Health::unavailable("Ray-traced lights are unavailable (see PhantomFX.log).");
    }

    if (!hasAnyTwin()) return Health::waiting("The game has not loaded its light shaders yet.");

    return ray_tracing::health();
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!active.load(std::memory_order_relaxed) || !call.indexed || call.instances != 1) return false;

    try
    {
        const auto* game = gameLightOf(shader.row());
        if (!game || isBeam(context, *game)) return false;

        // For path tracing alone, the game draws its lights while the rays do not light them.
        if (isForPathTracingOnly.load(std::memory_order_relaxed) && !ray_tracing::isPathTracing()) return false;

        // A smaller camera's light, as the intro mirror's: the rays trace the main view only.
        if (!viewportFillsTarget(context)) return false;

        // The stencil-marking draw writes no colour: drawn as the game does, but here, so that lamp
        // contact shadows (lamp_contact_shadows.hpp) do not trace a light the rays shadow.
        ComPtr<ID3D11BlendState> blend;
        float blendFactor[4]{};
        UINT sampleMask{};
        context->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
        D3D11_BLEND_DESC desc{};
        if (blend) blend->GetDesc(&desc);
        if (blend && desc.RenderTarget[0].RenderTargetWriteMask == 0)
        {
            context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
            return true;
        }

        auto noted = noteOf(context, game);
        if (!noted) return false;

        noted->volume = volumeOf(context, call);

        // The lighting draw keeps its stencil work, without its light: no pixel shader.
        context->PSSetShader(nullptr, nullptr, 0);
        context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
        context->PSSetShader(shader.object(), nullptr, 0);

        notes.note(reinterpret_cast<std::uint64_t>(context), [&](Notes& contextNotes) { contextNotes.lights.push_back(std::move(*noted)); });
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void noteDrawPoint(ID3D11DeviceContext* context) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    try
    {
        notes.note(reinterpret_cast<std::uint64_t>(context), [](Notes& contextNotes) { contextNotes.drawPoint = true; });
    }
    catch (...)
    {
    }
}

void recorded(std::uint64_t into, std::uint64_t from) noexcept
{
    notes.recorded(into, from);
}

void forget(std::uint64_t contextOrList) noexcept
{
    notes.forget(contextOrList);
}

std::vector<TracedLight> takeForTrace(const SceneConstants& camera) noexcept
{
    std::vector<TracedLight> lights;
    if (!active.load(std::memory_order_relaxed)) return lights;

    try
    {
        notes.withWaiting([&](auto&& waiting) {
            std::vector<Light*> untraced;
            std::vector<const DrawnLight*> drawn;
            for (auto& noted : waiting)
                for (auto& light : noted.lights)
                    if (!light.traced)
                    {
                        untraced.push_back(&light);
                        drawn.push_back(&light.drawn);
                    }

            const auto choice = light_choice::choose(drawn, camera);
            otherFrame += choice.otherFrame;
            overCap += choice.overCap;
            for (const auto index : choice.taken)
            {
                auto& light = *untraced[index];
                light.traced = true;
                auto rays = light_choice::forRays(light.drawn, volumeBoundsOf(light.drawn));
                lights.push_back({rays, rays.isMasked ? light.resources[MaskSlot] : nullptr});
            }
        });
    }
    catch (...)
    {
        lights.clear();
    }

    return lights;
}

void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    auto noted = notes.take(list);
    if (!noted) return;

    std::lock_guard lock(resourceMutex);
    if (context != immediate) return;

    try
    {
        for (auto& light : noted->lights) frameLights.push_back(std::move(light));
    }
    catch (...)
    {
    }

    // A list with a draw point comes after the game's lights, and waits for the rays: the lights so far are
    // drawn before it. Never at once in a later list: that list has not run yet, so neither have the copies
    // of its lights' volumes, nor has the immediate context waited for the rays a later kick traced. A
    // second camera, as the intro's mirror, draws its lights after the first camera's draw point.
    if (!noted->drawPoint) return;

    drawPointRan = true;
    drawFrameLights();
}

void present() noexcept
{
    bool sawDrawPoint{};
    {
        std::lock_guard lock(resourceMutex);
        lost += frameLights.size();
        frameLights.clear();
        sawDrawPoint = drawPointRan;
        drawPointRan = false;
        if (immediate) readVolumeBounds();
    }

    // Notes of lists that never ran go, as do a context's left unfinished.
    expiredNotes += notes.present();

    bool on = wantedRunning;
    {
        std::lock_guard lock(resourceMutex);
        on = on && fullScreenShader && !pipelineFailed;
    }

    on = on && hasAnyTwin();
    enabled = on;
    active = on && sawDrawPoint;
    if constexpr (DiagnosticLog) logLights();
}
}
