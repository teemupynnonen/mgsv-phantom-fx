// The global illumination's indirect light (docs/DEVELOPMENT.md, "Global illumination"), as path tracers
// with a radiance cache work it out:
// - Rays from each pixel of a grid half the render size each way (1, 2 or 4, the ray quality, twice on
//   foliage), from one render pixel of its 2 x 2 chosen by depth and kept on its surface (PickOf),
//   cosine-weighted from the surface the depth buffer gives (TraceLight).
// - Where it hits, the hit is lit afresh every frame: a shadow ray to a point of the sun's disc, and
//   one to a point or spot light picked in proportion to its light there (weighted reservoir
//   sampling). Its bounce ray takes the sky's light where it gets away, and otherwise the light of the
//   surface it reaches from a world-space radiance cache: AMD's two-level hash grid of GI-1.2 (Capsaicin,
//   MIT; licenses/Capsaicin-LICENSE.txt), whose cells hold the light at second hits and, through their
//   own rays into other cells, every bounce after. The cache only ends the path, where NVIDIA's SHaRC
//   sits in a path tracer: its lag (a cell takes 1 / 16 of a frame's light) and its coarseness come
//   after a bounce, not in the light a pixel sees first.
// - Foliage's light mostly from cells in the world that keep it over the frames (RememberFoliageLight).
// - NVIDIA's RELAX denoiser in its spherical harmonics form (gi_denoiser.cpp) at the same grid, given the
//   motion without the upscaler's jitter and the game's foliage as strands, then the render size from the four light pixels around each render pixel, weighed by how alike their surfaces
//   are and resolved against the render pixel's own normal (CompositeLight).
// - With DLSS Ray Reconstruction, which denoises the image itself and asks for samples apart at every
//   render pixel, the grid is the render size, a ray a pixel, and the light goes out as the rays found it,
//   without the denoiser or the foliage memory (PerRenderPixel).
//
// Where it deviates from a path tracer, and why:
// - A hit's albedo is the G-buffer's where the hit is on screen; otherwise the game's albedo texture
//   for a model's mesh (taken from its G-buffer draw, engine_scene.hpp), and the mesh's mean albedo,
//   learnt from the screen (LearnAlbedo), for the rest.
// - The lights are the sun and the sky, as the game's own sky light gives them, and the frame's point
//   and spot lights; with path tracing also its self-lit meshes (SampledEmitterLight). Specular light is
//   left out of the cache: the game keeps its own, and path tracing's rays bring theirs.
// - Rays from a surface skip copies of it just above (gi_common.hlsli, "Copies of a surface").
// - A hash grid tile is keyed by the side of the surface a ray hits (the ray's main axis and its
//   sign), not by its finely quantised direction: diffuse light needs no more, and the grid fills
//   up far less (a hit without room gives no light).
// - Only the first few hits of a cell each frame bring it light (HashGridCache_QueueLight); dozens a
//   frame land in most cells and a cell keeps 16 samples.
#include "gi_hit_light.hlsli"
#include "NRD.hlsli"

// ---- Bindings, after gi_common.hlsli's and gi_hit_light.hlsli's ----

RWTexture2D<float> g_Light_ViewZ : register(u7);            // The denoiser's guides: NRD's IN_VIEWZ,
RWTexture2D<float4> g_Light_NormalRoughness : register(u8); // IN_NORMAL_ROUGHNESS,
RWTexture2D<float> g_Light_LastViewZ : register(u30);       // g_Light_ViewZ as the last frame wrote it (PrepareGuides)
RWTexture2D<uint> g_Light_Pick : register(u35);             // Which render pixel of its 2 x 2 each light pixel's sample is from (PickOf)
RWTexture2D<unorm float4> g_Light_Validation : register(u36); // NRD's validation layer (OUT_VALIDATION), with its debug view (unorm: read as a UAV)
RWTexture2D<float4> g_Light_Motion : register(u9);          // IN_MV: the last frame's uv minus this one's, and view z's change
RWTexture2D<float4> g_Light_Noisy[2] : register(u10);       // u10, u11: NRD's IN_DIFF_SH0 and SH1
RWTexture2D<float4> g_Light_Denoised[2] : register(u12);    // u12, u13: NRD's OUT_DIFF_SH0 and SH1
RWTexture2D<float4> g_Light_Partial : register(u14);        // A ray's light but its bounce's cache: rgb; w its hit distance
RWTexture2D<uint4> g_Light_Path : register(u15);            // x its bounce hit's cell (kGI1_InvalidId for none); y its direction (packNormal);
                                                            // zw the bounce's throughput (packHalf4)

// The hit distance the denoiser gets for a ray that reaches the sky.
#define MISS_DISTANCE 1000.0f

// ---- The game's frame and the light's grid ----

uint2 LightSize()
{
    return g_Frame.lightLayout.xy;
}

// For DLSS Ray Reconstruction: a light pixel is a render pixel, and nothing is denoised here.
bool PerRenderPixel()
{
    return g_Frame.reconstruction.x != 0u;
}

// Render pixel `index` (x + 2 y) of a light pixel's 2 x 2; the light pixel itself per render pixel.
uint2 RenderPixelAt(uint2 lightPixel, uint index)
{
    if (PerRenderPixel()) return lightPixel;

    return min(lightPixel * 2 + uint2(index & 1u, index >> 1), g_BufferDimensions - 1);
}

// A light pixel's depth pick of its 2 x 2 (RenderPixelAt): the nearest of the four on light pixels of one
// checkerboard colour, the farthest on the other, as half-resolution effects downsample depth. With the top
// left, a wire, a fence or a grass blade under a pixel wide had it one frame and what lay behind the next as
// the upscaler's jitter moved them, and the denoiser started their light over every frame, the camera still
// (2026-10-25).
uint DepthPickOf(uint2 lightPixel)
{
    bool nearest = ((lightPixel.x + lightPixel.y) & 1u) == 0u;
    uint chosen = 0;
    float chosenDepth = g_Depth.Load(int3(RenderPixelAt(lightPixel, 0), 0));
    for (uint i = 1; i < 4; ++i)
    {
        float depth = g_Depth.Load(int3(RenderPixelAt(lightPixel, i), 0));
        // Reversed Z: the nearer, the greater.
        if (nearest ? depth > chosenDepth : depth < chosenDepth)
        {
            chosen = i;
            chosenDepth = depth;
        }
    }

    return chosen;
}

// The render pixel a light pixel's sample is from this frame, as PrepareGuides chose it (PickOf).
uint2 RenderPixelOf(uint2 lightPixel)
{
    if (PerRenderPixel()) return lightPixel;

    return RenderPixelAt(lightPixel, g_Light_Pick[lightPixel]);
}

// ---- Utilities ----

[numthreads(1, 1, 1)]
void ClearCounters()
{
    // The previous frame's packed tiles stay: PurgeTiles reads them this frame.
    g_Counters[PACKED_TILE_COUNT] = 0;
    for (uint counter = GI_COUNTER_UPDATE_TILES; counter < GI_COUNTER_COUNT; ++counter) g_Counters[counter] = 0;
}

// g_Dispatch: x counter, y threads (or tiles) a group handles, z argument slot, w a second counter to add.
[numthreads(1, 1, 1)]
void GenerateDispatch()
{
    uint count = g_Counters[g_Dispatch.x];
    if (g_Dispatch.w != GI_COUNTER_NONE) count += g_Counters[g_Dispatch.w];
    g_DispatchArgs[g_Dispatch.z] = uint4((count + g_Dispatch.y - 1) / g_Dispatch.y, 1, 1, 0);
}

// ---- The light's rays ----

// How far a still render pixel's surface lies from what the light grid held where it was last frame, in
// relative view depth; 1 where that cannot be told (the sky, a moving mesh, off screen last frame).
float LastFrameMismatch(uint2 pixel)
{
    float depth = g_Depth.Load(int3(pixel, 0));
    float3 encoded = g_Normal.Load(int3(pixel, 0)).xyz;
    float2 moved;
    if (depth <= 0.0f || dot(encoded, encoded) == 0.0f || ObjectMotion(int2(pixel), moved)) return 1.0f;

    float2 motion;
    float previousViewZ;
    CameraMotionOf(pixel, depth, motion, previousViewZ);
    float2 lastUv = (pixel + 0.5f) / g_BufferDimensions + motion;
    if (any(lastUv <= 0.0f) || any(lastUv >= 1.0f)) return 1.0f;

    float lastViewZ = g_Light_LastViewZ[min(uint2(lastUv * LightSize()), LightSize() - 1)];
    return abs(lastViewZ - previousViewZ) / max(previousViewZ, 1e-4f);
}

// Which render pixel of its 2 x 2 (RenderPixelAt) a light pixel takes its sample from: the depth pick,
// unless its surface is not the one the light grid held there last frame; then the one whose surface is,
// if one is. As the upscaler's jitter moved an edge across a 2 x 2, the depth pick went from one side of
// it to the other, and the denoiser started the light over there every frame, the camera still: about
// five times the young history in grass (2026-10-29). 3% passes slopes across a 2 x 2 and catches an
// edge's foreground against its background. Against the 2 x 2 light pixels around where it was, as NRD's
// own test takes them, it looked worse in game. A moving mesh keeps the depth pick.
uint PickOf(uint2 lightPixel)
{
    const float threshold = 0.03f;
    uint pick = DepthPickOf(lightPixel);
    uint2 depthPick = RenderPixelAt(lightPixel, pick);
    float2 moved;
    if (ObjectMotion(int2(depthPick), moved) || LastFrameMismatch(depthPick) < threshold) return pick;

    float best = threshold;
    for (uint i = 0; i < 4; ++i)
    {
        float mismatch = LastFrameMismatch(RenderPixelAt(lightPixel, i));
        if (mismatch < best)
        {
            best = mismatch;
            pick = i;
        }
    }

    return pick;
}

// The denoiser's guides for each light pixel: its view depth (the sky's beyond the denoising range), its
// normal and material (foliage's or not), and where it was on screen last frame, without the upscaler's
// jitter: the camera's motion, or a moving mesh's from the game's velocity, with the view depth's change by
// the camera's.
[numthreads(8, 8, 1)]
void PrepareGuides(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= LightSize())) return;

    uint pick = PickOf(did);
    g_Light_Pick[did] = pick;
    uint2 pixel = RenderPixelAt(did, pick);
    float depth = g_Depth.Load(int3(pixel, 0));
    float4 packedNormal = g_Normal.Load(int3(pixel, 0));
    float3 encoded = packedNormal.xyz;
    if (depth <= 0.0f || dot(encoded, encoded) == 0.0f)
    {
        g_Light_ViewZ[did] = GI_LIGHT_SKY_VIEW_Z;
        g_Light_NormalRoughness[did] = NRD_FrontEnd_PackNormalAndRoughness(float3(0.0f, 0.0f, 1.0f), 1.0f, 0.0f);
        g_Light_Motion[did] = 0.0f;
        return;
    }

    float viewZ = toLinearDepth(depth, g_NearFar);
    float2 motion;
    float previousViewZ;
    CameraMotionOf(pixel, depth, motion, previousViewZ);

    // NRD takes the motion without the jitter, as its projections (denoiserCamera in gi_renderer.cpp); the
    // game's velocity runs from this frame's jittered place to the last frame's unjittered one.
    float2 moved;
    bool movingMesh = ObjectMotion(int2(pixel), moved);
    if (movingMesh) motion = JitterUv(g_Frame.jitter.xy) - moved / g_BufferDimensions;

    // A moving mesh's view depth last frame: the last frame's where it was, when that lies within 10% of its own
    // now; else as if it stood still, which takes it as new where it was something else. As if it stood still
    // always, the change was off by the camera's movement where the camera follows it, as it does the player:
    // a few percent a frame, past NRD's 1%, and the player's light started over every frame (2026-10-25).
    float depthChange = previousViewZ - viewZ;
    if (movingMesh)
    {
        float2 lastUv = (did + 0.5f) / LightSize() + motion;
        if (all(lastUv > 0.0f) && all(lastUv < 1.0f))
        {
            float lastViewZ = g_Light_LastViewZ[min(uint2(lastUv * LightSize()), LightSize() - 1)];
            if (abs(lastViewZ - viewZ) < 0.1f * viewZ) depthChange = lastViewZ - viewZ;
        }
    }

    bool foliage = packedNormal.w < 0.5f;
    CountDiagnostic(GI_COUNTER_FOLIAGE, foliage);
    g_Light_ViewZ[did] = viewZ;
    g_Light_NormalRoughness[did] = NRD_FrontEnd_PackNormalAndRoughness(DecodeNormal(packedNormal), 1.0f, foliage ? GI_LIGHT_FOLIAGE_MATERIAL : 0.0f);
    g_Light_Motion[did] = float4(motion, depthChange, 0.0f);
}

// A light pixel's rays (the ray quality, 1, 2 or 4, twice on foliage): what each hits lit afresh, and its
// bounce, the sky's light or a cache cell's. The first ray's cell takes the hit's light and gives its own
// after the cache's update (ResolveLight); the others' cells give theirs as they stand, a frame old, and take none,
// the cache's buffers holding a hit a light pixel. Their light is averaged; their directions, weighed by
// it, give the denoiser's spherical harmonics (shorter than 1 where the rays' light spreads).
[numthreads(8, 8, 1)]
void TraceLight(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= LightSize())) return;

    uint2 pixel = RenderPixelOf(did);
    float depth = g_Depth.Load(int3(pixel, 0));
    float3 encoded = g_Normal.Load(int3(pixel, 0)).xyz;
    uint4 path = uint4(kGI1_InvalidId, 0, 0, 0);
    if (depth <= 0.0f || dot(encoded, encoded) == 0.0f)
    {
        g_Light_Partial[did] = 0.0f;
        g_Light_Path[did] = path;
        return;
    }

    float3 normal = DecodeNormal(float4(encoded, 0.0f));
    float3 world = DepthPosition(pixel, depth);
    Random random = MakeRandom(did.x + did.y * LightSize().x, g_FrameIndex);
    float3 origin = OffsetFromScreenSurface(world, normal);
    uint rays = clamp(g_Frame.lightRays.x, 1u, 4u);
    // Foliage (the normals' alpha under a half, PrepareGuides) takes twice: swaying and thin, it restarts its
    // denoiser history every few frames, so its light is mostly the frame's rays; shaded grass boiled (2026-10-25).
    if (g_Normal.Load(int3(pixel, 0)).w < 0.5f) rays *= 2u;
    float3 total = 0.0f, weightedDirection = 0.0f, firstDirection = 0.0f;
    float totalLuminance = 0.0f, totalDistance = 0.0f;
    for (uint ray = 0; ray < rays; ++ray)
    {
        float3 direction = mapToCosineHemisphere(random.rand2(), normal);
        if (ray == 0) firstDirection = direction;
        SceneHit hit = TraceFromSurface(origin, normal, CopyTolerance(world), direction, MAX_HIT_DISTANCE);
        CountDiagnostic(GI_COUNTER_HITS, hit.hit);
        CountDiagnostic(GI_COUNTER_NEAR_HITS, hit.hit && hit.distance < 0.25f);
        float3 light;
        float3 afterUpdate = 0.0f; // The first ray's cell's light as it stands, to weigh its direction
        if (!hit.hit)
        {
            light = SkyRadiance(direction);
            totalDistance += MISS_DISTANCE;
        }
        else
        {
            // The hit's own light, from the sun and a light.
            HashGridCache_Visibility visibility = VisibilityOf(hit, direction);
            HitMaterial material = HitMaterialOf(visibility, 0.0f, true);
            float3 hitWorld = material.world, hitNormal = material.geometric, albedo = material.albedo;
            bool leaf = material.isLeaf;
            float3 hitOrigin = OffsetFromTriangle(hitWorld, hitNormal);
            float tolerance = CopyTolerance(hitWorld);
            light = DiffuseRayEmission(visibility, material.emission) + HitOwnLight(material, -direction, hitOrigin, tolerance, random);
            MaxDiagnostic(GI_COUNTER_MAX_ALBEDO, albedo);
            MaxDiagnostic(GI_COUNTER_MAX_DIRECT, light);

            // Its bounce: the sky's light, or the cache's at what it reaches. The Lambert BRDF over the
            // cosine-weighted pdf is the albedo.
            float weight;
            float3 side = BounceSide(hitNormal, leaf, random, weight);
            float3 bounceDirection = mapToCosineHemisphere(random.rand2(), side);
            float3 bounceOrigin = all(side == hitNormal) ? hitOrigin : OffsetFromTriangle(hitWorld, side);
            SceneHit bounce = TraceFromSurface(bounceOrigin, side, tolerance, bounceDirection, MAX_HIT_DISTANCE);
            CountDiagnostic(GI_COUNTER_BOUNCE_HITS, bounce.hit);
            float3 throughput = albedo * weight;
            if (!bounce.hit) light += throughput * SkyRadiance(bounceDirection);
            else
            {
                float3 bounceHit = bounceOrigin + bounce.distance * bounceDirection;
                uint tileIndex;
                bool isNewTile;
                uint cellIndex = HashGridCache_InsertCell(bounceHit, bounceDirection, bounce.distance, tileIndex, isNewTile);
                CountDiagnostic(GI_COUNTER_FULL_BUCKETS, cellIndex == kGI1_InvalidId);
                if (cellIndex != kGI1_InvalidId)
                {
                    HashGridCache_TouchCell(tileIndex, isNewTile);
                    float3 cell = HashGridCache_FilteredDirect(cellIndex) + HashGridCache_FilteredIndirect(cellIndex);
                    if (ray == 0)
                    {
                        uint index = HashGridCache_Keep(GI_COUNTER_VISIBILITY0, 0, VisibilityOf(bounce, bounceDirection), cellIndex, did.x + did.y * LightSize().x);
                        HashGridCache_QueueLight(index, cellIndex);
                        path.x = cellIndex;
                        path.zw = packHalf4(float4(throughput / rays, 0.0f));
                        afterUpdate = throughput * cell;
                    }
                    else light += throughput * cell;
                }
            }

            totalDistance += hit.distance;
        }

        total += light;
        float rayLuminance = luminance(light + afterUpdate);
        weightedDirection += rayLuminance * direction;
        totalLuminance += rayLuminance;
    }

    // The self-lit meshes' light on the pixel's own surface, which its rays would rarely meet.
    total += rays * SampledEmitterLight(world, normal, origin, CopyTolerance(world), random);

    path.y = packNormal(totalLuminance > 0.0f ? weightedDirection / totalLuminance : firstDirection);
    g_Light_Partial[did] = float4(total / rays, totalDistance / rays);
    g_Light_Path[did] = path;
}

// Teaches each mesh its mean albedo from the screen: a ray from the camera finds which mesh a pixel
// shows. One light pixel of each 16 x 16 a frame, moving over them; counts stop at 4096, so a mesh's
// albedo settles.
[numthreads(64, 1, 1)]
void LearnAlbedo(uint did : SV_DispatchThreadID)
{
    uint2 tiles = (LightSize() + 15) / 16;
    if (did >= tiles.x * tiles.y) return;

    uint2 jitter = min(uint2(CalculateHaltonSequence(g_FrameIndex) * 16.0f), 15u);
    uint2 lightPixel = uint2(did % tiles.x, did / tiles.x) * 16 + jitter;
    if (any(lightPixel >= LightSize())) return;

    uint2 seed = RenderPixelOf(lightPixel);
    float depth = g_Depth.Load(int3(seed, 0));
    if (depth <= 0.0f) return;

    float3 world = DepthPosition(seed, depth);
    float3 offset = world - g_Eye;
    float seedDistance = sqrt(dot(offset, offset));
    SceneHit hit = TraceClosest(g_Eye, offset / seedDistance, 1.05f * seedDistance + 0.5f);
    if (!hit.hit || abs(hit.distance - seedDistance) > 0.03f * seedDistance + 0.05f) return;

    uint row = FirstGeometry(g_Instances[hit.instance]) + hit.geometry;
    if (g_GeometryAlbedo[row].w >= 4096) return;

    uint3 albedo = uint3(round(saturate(g_Albedo.Load(int3(seed, 0)).rgb) * 255.0f));
    InterlockedAdd(g_GeometryAlbedo[row].x, albedo.x);
    InterlockedAdd(g_GeometryAlbedo[row].y, albedo.y);
    InterlockedAdd(g_GeometryAlbedo[row].z, albedo.z);
    InterlockedAdd(g_GeometryAlbedo[row].w, 1);
}

// ---- Hash grid cache kernels ----

[numthreads(64, 1, 1)]
void PurgeTiles(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[PREVIOUS_PACKED_TILE_COUNT]) return;

    // A tile's age from the frame index, which saves decrementing every tile each frame.
    uint tileIndex = g_HashGridCache_PreviousPackedTileIndexBuffer[did];
    uint tileDecay = g_HashGridCache_DecayTileBuffer[tileIndex];
    tileDecay = g_FrameIndex < tileDecay ? (0xFFFFFFFFu - tileDecay) + g_FrameIndex + 1 : g_FrameIndex - tileDecay;
    if (tileDecay >= kHashGridCache_TileDecay)
    {
        g_HashGridCache_HashBuffer[tileIndex] = 0;
        return;
    }

    uint packedTileIndex;
    InterlockedAdd(g_Counters[PACKED_TILE_COUNT], 1, packedTileIndex);
    g_HashGridCache_PackedTileIndexBuffer[packedTileIndex] = tileIndex;
}

// GI-1.2's multibounce: some second hits' cells trace on to a third hit's cell. A ray that gets away is
// kept too, as a sample without light: a cell's direct light has the sky already (HitDirectLight), so its
// indirect light is what the surfaces around it give, over all its rays. GI-1.2 left those rays out, which
// made the indirect light 1.4 times too bright in the median of the logs, twice in open ground (from the
// share of multibounce rays that hit, 2026-10-30).
[numthreads(32, 1, 1)]
void PopulateMultibounceCells(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_VISIBILITY0]) return;

    Random random = MakeRandom(did, g_FrameIndex);
    if (random.rand() <= HASH_DISCARD_MULTIBOUNCE) return;

    HashGridCache_Visibility visibility = HashGridCache_UnpackVisibility(g_HashGridCache_VisibilityBuffer[did]);
    float3 world, normal;
    HitSurface(visibility, world, normal);
    float3 albedo = HitAlbedo(world, visibility);

    // Cosine-weighted: the Lambert BRDF over its pdf is the albedo.
    float3 direction = mapToCosineHemisphere(random.rand2(), normal);
    float3 origin = OffsetFromTriangle(world, normal);
    SceneHit hit = TraceFromSurface(origin, normal, CopyTolerance(world), direction, MAX_HIT_DISTANCE);
    CountDiagnostic(GI_COUNTER_MULTIBOUNCE_AWAY, !hit.hit);
    uint cellIndex = kGI1_InvalidId;
    if (hit.hit)
    {
        float3 hitPosition = origin + hit.distance * direction;
        uint tileIndex;
        bool isNewTile;
        cellIndex = HashGridCache_InsertCell(hitPosition, direction, hit.distance, tileIndex, isNewTile);
        // A hit without room in the cache is left out: its light is unknown, not none.
        if (cellIndex == kGI1_InvalidId) return;

        HashGridCache_TouchCell(tileIndex, isNewTile);
    }

    // Third hits follow the second ones' room; Capsaicin divides by the chance to trace, but a cell
    // averages only the samples that were traced, which made its indirect light 1 / 0.3 too bright.
    uint index = HashGridCache_Keep(GI_COUNTER_VISIBILITY1, g_Frame.lightLayout.z, VisibilityOf(hit, direction), cellIndex, did);
    g_HashGridCache_MultibounceInfoBuffer[index] = packHalf4(float4(albedo, 0.0f));
    if (hit.hit) HashGridCache_QueueLight(g_Frame.lightLayout.z + index, cellIndex);
}

// The direct light of the queued hits, second and third, into their cells.
[numthreads(32, 1, 1)]
void PopulateCells(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_LIGHTS]) return;

    uint visibilityIndex = g_HashGridCache_LightBuffer[did];
    HashGridCache_Visibility visibility = HashGridCache_UnpackVisibility(g_HashGridCache_VisibilityBuffer[visibilityIndex]);
    float3 world, normal;
    HitSurface(visibility, world, normal);
    float3 albedo = HitAlbedo(world, visibility);
    Random random = MakeRandom(did + 0x9E3779B9u, g_FrameIndex);
    float3 lighting = HitDirectLight(world, normal, albedo, HitIsLeaf(visibility), random);
    SceneTriangle hitTriangle = LoadTriangle(visibility.instanceIndex, visibility.geometryIndex, visibility.primitiveIndex);
    if (hitTriangle.geometry.flags & GI_GEOMETRY_EMISSIVE) lighting += DiffuseRayEmission(visibility, EmissionAt(hitTriangle, visibility.barycentrics));
    MaxDiagnostic(GI_COUNTER_MAX_ALBEDO, albedo);
    MaxDiagnostic(GI_COUNTER_MAX_DIRECT, lighting);
    if (!(dot(lighting, lighting) > 0.0f)) return;

    // The sample's count went in with its claim (HashGridCache_QueueLight).
    uint cellIndex = g_HashGridCache_VisibilityCellBuffer[visibilityIndex];
    uint4 quantized = HashGridCache_QuantizeRadiance(RemoveNaNs(lighting));
    InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 0], quantized.x);
    InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 1], quantized.y);
    InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 2], quantized.z);
}

// Tiles of 8 x 8 cells: one 8 x 8 group a tile.
groupshared uint2 lds_UpdateTiles_ValueBuffer[8][8];
groupshared uint2 lds_UpdateTiles_ValueIndirectBuffer[8][8];

// `frameSamples`: the most samples the frame's update holds; a cell's direct light has more claims than samples.
float4 AccumulateCell(RWStructuredBuffer<uint2> values, RWStructuredBuffer<uint> updates, uint cellIndex, float maxSamples, uint frameSamples)
{
    float4 radiance = unpackHalf4(values[cellIndex]);
    uint4 sums = TakeQuantizedRadiance(updates, 4 * cellIndex);
    float4 added = HashGridCache_RecoverRadiance(uint4(sums.xyz, min(sums.w, frameSamples)));
    float sampleCount = min(radiance.w + added.w, maxSamples);
    radiance /= max(radiance.w, 1.0f);
    added /= max(added.w, 1.0f);
    radiance = radiance.w <= 0.0f ? added : lerp(radiance, added, 1.0f / sampleCount);
    // The sample count rides along as a hint for how far to filter.
    radiance *= sampleCount;
    return radiance;
}

[numthreads(8, 8, 1)]
void UpdateTiles(uint groupId : SV_GroupID, uint2 threadId : SV_GroupThreadID)
{
    // A group covers one tile of 8 x 8 cells (the tile cell ratio).
    if (groupId >= g_Counters[GI_COUNTER_UPDATE_TILES]) return;

    uint tileIndex = g_HashGridCache_UpdateTileBuffer[groupId];
    uint cellIndex = HashGridCache_CellIndex(threadId, tileIndex, 0);

    uint2 packedDirect = packHalf4(AccumulateCell(g_HashGridCache_ValueBuffer, g_HashGridCache_UpdateCellValueBuffer, cellIndex, HASH_MAX_SAMPLE_COUNT,
        HASH_CELL_SAMPLES_PER_FRAME));
    uint2 packedIndirect = packHalf4(AccumulateCell(g_HashGridCache_ValueIndirectBuffer, g_HashGridCache_UpdateCellValueIndirectBuffer, cellIndex,
        HASH_MAX_MULTIBOUNCE_SAMPLE_COUNT, 0xFFFFFFFFu));
    float4 storedDirect = unpackHalf4(packedDirect), storedIndirect = unpackHalf4(packedIndirect);
    MaxDiagnostic(GI_COUNTER_MAX_CELL, storedDirect.rgb / max(storedDirect.w, 1.0f));
    MaxDiagnostic(GI_COUNTER_MAX_INDIRECT, storedIndirect.rgb / max(storedIndirect.w, 1.0f));
    lds_UpdateTiles_ValueBuffer[threadId.x][threadId.y] = packedDirect;
    lds_UpdateTiles_ValueIndirectBuffer[threadId.x][threadId.y] = packedIndirect;
    g_HashGridCache_ValueBuffer[cellIndex] = packedDirect;
    g_HashGridCache_ValueIndirectBuffer[cellIndex] = packedIndirect;

    // Mips 1 to 3: box filters of the one below.
    [unroll]
    for (uint mip = 1; mip < 4; ++mip)
    {
        GroupMemoryBarrierWithGroupSync();
        uint step = 1u << (mip - 1);
        if (g_Frame.hashTileSizes[mip] > 0 && all((threadId % (step * 2)) == 0))
        {
            float4 direct = unpackHalf4(lds_UpdateTiles_ValueBuffer[threadId.x][threadId.y])
                + unpackHalf4(lds_UpdateTiles_ValueBuffer[threadId.x + step][threadId.y])
                + unpackHalf4(lds_UpdateTiles_ValueBuffer[threadId.x][threadId.y + step])
                + unpackHalf4(lds_UpdateTiles_ValueBuffer[threadId.x + step][threadId.y + step]);
            float4 indirect = unpackHalf4(lds_UpdateTiles_ValueIndirectBuffer[threadId.x][threadId.y])
                + unpackHalf4(lds_UpdateTiles_ValueIndirectBuffer[threadId.x + step][threadId.y])
                + unpackHalf4(lds_UpdateTiles_ValueIndirectBuffer[threadId.x][threadId.y + step])
                + unpackHalf4(lds_UpdateTiles_ValueIndirectBuffer[threadId.x + step][threadId.y + step]);
            uint mipCell = HashGridCache_CellIndex(threadId, tileIndex, mip);
            g_HashGridCache_ValueBuffer[mipCell] = packHalf4(direct);
            g_HashGridCache_ValueIndirectBuffer[mipCell] = packHalf4(indirect);
            lds_UpdateTiles_ValueBuffer[threadId.x][threadId.y] = packHalf4(direct);
            lds_UpdateTiles_ValueIndirectBuffer[threadId.x][threadId.y] = packHalf4(indirect);
        }
    }
}

// Third hits' cells' light, filtered, into their second hits' cells' indirect light; it takes effect next
// frame. Their indirect light goes along with their direct light, so the cache feeds its own light back: a
// bounce more each frame, adding up to all of them, as SHaRC's and DDGI's caches do. A bounce keeps its
// albedo's share of the light, less where rays get away, so the sum settles. GI-1.2 passed on the direct
// light alone: three bounces in all.
[numthreads(32, 1, 1)]
void UpdateMultibounceCells(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_VISIBILITY1]) return;

    uint visibilityIndex = g_Frame.lightLayout.z + did;
    uint cellIndex = g_HashGridCache_VisibilityCellBuffer[visibilityIndex];
    uint baseCellIndex = g_HashGridCache_VisibilityCellBuffer[g_HashGridCache_VisibilityQueryBuffer[visibilityIndex]];
    float3 radiance = 0.0f; // None for a ray that got away
    if (cellIndex != kGI1_InvalidId)
    {
        float3 cell = HashGridCache_FilteredDirect(cellIndex) + HashGridCache_FilteredIndirect(cellIndex);
        radiance = cell * unpackHalf4(g_HashGridCache_MultibounceInfoBuffer[did]).rgb;
    }
    MaxDiagnostic(GI_COUNTER_MAX_BOUNCE, radiance);
    uint4 quantized = HashGridCache_QuantizeRadiance(RemoveNaNs(radiance));
    if (dot(radiance, radiance) > 0.0f)
    {
        InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 0], quantized.x);
        InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 1], quantized.y);
        InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 2], quantized.z);
    }
    InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 3], quantized.w);
}

// ---- Foliage's light by world position ----

// Grass and leaves sway, and the denoiser starts their light over every few frames, so shaded foliage boiled
// with each frame's rays (2026-10-25); a cell in the world stays where it is. Cells about 8 light pixels across where they lie (power-of-two sizes, 6.25 cm at the least), in
// a hash table of buckets (GI_FOLIAGE_MEMORY_SLOTS, GI_FOLIAGE_MEMORY_BUCKET). Each frame a foliage light pixel
// adds its light to its cell, found at a point jittered by up to half a cell each way (blending neighbouring
// cells over the frames), and takes FOLIAGE_MEMORY_SHARE of its light from the cell as the last frame left it,
// once the cell has 8 frames. UpdateFoliageMemory blends the frame's light into each cell over half a second
// (FOLIAGE_MEMORY_FRAMES) and frees the cells no foliage took for 2 seconds.
RWStructuredBuffer<uint> g_FoliageMemory_Keys : register(u31);    // A cell's key, 0 for a free slot
RWStructuredBuffer<uint> g_FoliageMemory_Touched : register(u32); // The frame foliage last took the cell
RWStructuredBuffer<uint> g_FoliageMemory_Added : register(u33);   // The frame's light into it, four a slot: rgb quantised
                                                                  // (HashGridCache_QuantizeRadiance), and the light pixels'
RWStructuredBuffer<uint2> g_FoliageMemory_Light : register(u34);  // Its light (packHalf4): rgb, and w its frames

#define FOLIAGE_MEMORY_SHARE 0.8f
#define FOLIAGE_MEMORY_FRAMES 30.0f
#define FOLIAGE_MEMORY_IDLE_FRAMES 120u

// The slot of the cell at `world`, found at a point `jitter` (-0.5 to 0.5 each way) cells off; kGI1_InvalidId
// when its bucket is full.
uint FoliageMemorySlot(float3 world, float3 jitter)
{
    float size = max(distance(g_Eye, world) * g_Frame.rayCone.x * 16.0f, 1.0f / 16.0f);
    int level = int(ceil(log2(size)));
    float cellSize = exp2(level);
    uint3 c = asuint(int3(floor(world / cellSize + jitter)));
    uint l = asuint(level);
    uint key = max(1u, xxHash(l + xxHash(c.x + xxHash(c.y + xxHash(c.z)))));
    uint bucket = pcgHash(l + pcgHash(c.x + pcgHash(c.y + pcgHash(c.z)))) % (GI_FOLIAGE_MEMORY_SLOTS / GI_FOLIAGE_MEMORY_BUCKET);
    for (uint i = 0; i < GI_FOLIAGE_MEMORY_BUCKET; ++i)
    {
        uint slot = bucket * GI_FOLIAGE_MEMORY_BUCKET + i;
        uint previous;
        InterlockedCompareExchange(g_FoliageMemory_Keys[slot], 0u, key, previous);
        if (previous == 0u || previous == key) return slot;
    }

    return kGI1_InvalidId;
}

// A light pixel's light with its cell's, if it shows foliage: adds its own to the cell's frame.
float3 RememberFoliageLight(uint2 lightPixel, float3 radiance)
{
    uint2 pixel = RenderPixelOf(lightPixel);
    float depth = g_Depth.Load(int3(pixel, 0));
    float4 packedNormal = g_Normal.Load(int3(pixel, 0));
    if (depth <= 0.0f || dot(packedNormal.xyz, packedNormal.xyz) == 0.0f || packedNormal.w >= 0.5f) return radiance;

    Random random = MakeRandom(lightPixel.x + lightPixel.y * LightSize().x + g_Frame.lightLayout.z, g_FrameIndex);
    float3 jitter = float3(random.rand2(), random.rand()) - 0.5f;
    uint slot = FoliageMemorySlot(DepthPosition(pixel, depth), jitter);
    CountDiagnostic(GI_COUNTER_FOLIAGE_MEMORY_FULL, slot == kGI1_InvalidId);
    if (slot == kGI1_InvalidId) return radiance;

    uint4 quantized = HashGridCache_QuantizeRadiance(RemoveNaNs(radiance));
    InterlockedAdd(g_FoliageMemory_Added[4 * slot + 0], quantized.x);
    InterlockedAdd(g_FoliageMemory_Added[4 * slot + 1], quantized.y);
    InterlockedAdd(g_FoliageMemory_Added[4 * slot + 2], quantized.z);
    InterlockedAdd(g_FoliageMemory_Added[4 * slot + 3], quantized.w);
    g_FoliageMemory_Touched[slot] = g_FrameIndex;
    float4 kept = unpackHalf4(g_FoliageMemory_Light[slot]);
    return lerp(radiance, kept.rgb, FOLIAGE_MEMORY_SHARE * saturate(kept.w / 8.0f));
}

[numthreads(64, 1, 1)]
void UpdateFoliageMemory(uint did : SV_DispatchThreadID)
{
    if (did >= GI_FOLIAGE_MEMORY_SLOTS || g_FoliageMemory_Keys[did] == 0u) return;

    if (g_FoliageMemory_Added[4 * did + 3] == 0u)
    {
        if (g_FrameIndex - g_FoliageMemory_Touched[did] > FOLIAGE_MEMORY_IDLE_FRAMES)
        {
            g_FoliageMemory_Keys[did] = 0u;
            g_FoliageMemory_Light[did] = uint2(0, 0);
        }
        return;
    }

    uint4 added = TakeQuantizedRadiance(g_FoliageMemory_Added, 4 * did);
    float4 kept = unpackHalf4(g_FoliageMemory_Light[did]);
    float frames = min(kept.w + 1.0f, FOLIAGE_MEMORY_FRAMES);
    float3 light = HashGridCache_RecoverRadiance(added).rgb / added.w;
    g_FoliageMemory_Light[did] = packHalf4(float4(lerp(kept.rgb, light, 1.0f / frames), frames));
}

// ---- Denoising and the render size ----

// Each ray's light with its bounce's cell, after the cache's update, and foliage's with its cell in the
// foliage memory, as the denoiser takes it.
[numthreads(8, 8, 1)]
void ResolveLight(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= LightSize())) return;

    float4 partial = g_Light_Partial[did];
    uint4 path = g_Light_Path[did];
    float3 radiance = partial.rgb;
    if (path.x != kGI1_InvalidId)
        radiance += unpackHalf4(path.zw).rgb * (HashGridCache_FilteredDirect(path.x) + HashGridCache_FilteredIndirect(path.x));

    MaxDiagnostic(GI_COUNTER_MAX_TRACED, radiance);
    // Ray Reconstruction takes the light as the pixel's own rays found it. The foliage memory would mix
    // in other pixels' and other frames' light, and Ray Reconstruction assumes samples apart (NVIDIA's
    // guide, "Sampling and Noise").
    if (PerRenderPixel())
    {
        g_Light_Noisy[0][did] = float4(radiance, partial.w);
        return;
    }

    radiance = RememberFoliageLight(did, radiance);

    // The rays' direction, at most 1 long (TraceLight).
    float3 direction = unpackNormal(path.y);
    float4 second;
    g_Light_Noisy[0][did] = RELAX_FrontEnd_PackSh(radiance, partial.w, direction * rsqrt(max(dot(direction, direction), 1.0f)), second, true);
    g_Light_Noisy[1][did] = second;
}

// A light pixel's denoised light at a surface of `normal`, its own normal being `ownNormal`: its spherical
// harmonics resolved against the normal (NRD_SH_ResolveDiffuse), scaled to keep the light it has at its own.
float3 DenoisedLight(uint2 lightPixel, float3 normal, float3 ownNormal)
{
    NRD_SG sg = RELAX_BackEnd_UnpackSh(g_Light_Denoised[0][lightPixel], g_Light_Denoised[1][lightPixel].xyz);
    float3 color = NRD_SG_ExtractColor(sg);
    float own = luminance(NRD_SH_ResolveDiffuse(sg, ownNormal));
    float here = luminance(NRD_SH_ResolveDiffuse(sg, normal));
    return own > 1e-6f ? color * clamp(here / own, 0.5f, 2.0f) : color;
}

// Diagnostics, blended over the image by its alpha: the denoiser's history at each render pixel
// (DebugView::LightHistory, phantom_fx_settings.hpp), red where it started the light over within three
// frames, yellow under ten; or NRD's validation layer (DebugView::LightValidation).
RWTexture2D<float4> g_LightDebug : register(u79);

float4 HistoryColor(float frames)
{
    if (frames < 3.0f) return float4(1.0f, 0.1f, 0.1f, 0.75f);
    if (frames < 10.0f) return float4(1.0f, 0.85f, 0.1f, 0.5f);
    return 0.0f;
}

void WriteLightDebug(uint2 pixel, float4 history)
{
    if (g_Frame.lightDiagnostics.z == GI_LIGHT_DEBUG_VALIDATION) g_LightDebug[pixel] = g_Light_Validation[min(pixel / 2, LightSize() - 1)];
    else if (g_Frame.lightDiagnostics.z == GI_LIGHT_DEBUG_HISTORY) g_LightDebug[pixel] = history;
}

// The light at every render pixel, irradiance over pi times the exposure: from the four light pixels
// around it, as a bilinear filter weighs them, times how alike their view depth and normal are to its own.
[numthreads(8, 8, 1)]
void CompositeLight(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_BufferDimensions)) return;

    float depth = g_Depth.Load(int3(did, 0));
    float3 encoded = g_Normal.Load(int3(did, 0)).xyz;
    if (depth <= 0.0f || dot(encoded, encoded) == 0.0f)
    {
        g_Output[did] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        WriteLightDebug(did, 0.0f);
        return;
    }

    // For Ray Reconstruction, the render pixel's own rays' light (ResolveLight).
    if (PerRenderPixel())
    {
        float3 traced = g_Light_Noisy[0][did].rgb;
        MaxDiagnostic(GI_COUNTER_MAX_OUTPUT, traced);
        g_Output[did] = float4(RemoveNaNs(traced) * g_Frame.nearFar.w, 1.0f); // Ray Reconstruction spreads one infinite pixel into a white blob
        return;
    }

    float3 normal = DecodeNormal(float4(encoded, 0.0f));
    float viewZ = toLinearDepth(depth, g_NearFar);
    // Light pixel p's ray left from render pixel 2p: an even render pixel is a light pixel, an odd one
    // halfway between two.
    float2 position = did * 0.5f;
    int2 base = int2(floor(position));
    float2 f = position - base;
    float3 total = 0.0f, nearest = 0.0f;
    float nearestHistory = 0.0f; // RELAX's history length in frames, with the light (OUT_DIFF_SH0.w)
    float weight = 0.0f, nearestDepth = FLT_MAX;
    for (uint i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1, i >> 1);
        uint2 tap = uint2(clamp(base + offset, 0, int2(LightSize()) - 1));
        float tapViewZ = g_Light_ViewZ[tap];
        if (tapViewZ >= GI_LIGHT_DENOISING_RANGE) continue;

        float3 tapNormal = DecodeNormal(g_Normal.Load(int3(RenderPixelOf(tap), 0)));
        float3 light = DenoisedLight(tap, normal, tapNormal);
        MaxDiagnostic(GI_COUNTER_MAX_DENOISED, light);
        float depthDifference = abs(tapViewZ - viewZ) / viewZ;
        if (depthDifference < nearestDepth)
        {
            nearestDepth = depthDifference;
            nearest = light;
            nearestHistory = g_Light_Denoised[0][tap].w;
        }

        float bilinear = (offset.x ? f.x : 1.0f - f.x) * (offset.y ? f.y : 1.0f - f.y);
        float w = bilinear * saturate(1.0f - depthDifference / 0.05f) * pow(saturate(dot(tapNormal, normal)), 8.0f);
        total += w * light;
        weight += w;
    }

    // No light pixel of the same surface around (a thin edge): the one nearest in depth.
    float3 light = weight > 1e-4f ? total / weight : nearest;
    WriteLightDebug(did, HistoryColor(nearestHistory));
    MaxDiagnostic(GI_COUNTER_MAX_OUTPUT, light);
    g_Output[did] = float4(light * g_Frame.nearFar.w, 1.0f);
}
