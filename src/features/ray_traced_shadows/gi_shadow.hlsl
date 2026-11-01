// The sun's ray-traced shadows (docs/DEVELOPMENT.md, "Ray-traced shadows"): a shadow ray from each
// pixel towards a point on the sun's disc, then the temporal pass of AMD's FidelityFX Shadow
// Denoiser (gi_shadow.hlsli); gi_shadow_filter.hlsl has its spatial passes.
//
// Cut-out geometry with the game's alpha (GI_GEOMETRY_ALPHA_TEXTURE) casts its own shadow, holes and
// all. The game's shadow map keeps the shadows of cut-out geometry without it where it reaches: the
// rays let the sun through it there, and the sun pass takes the darker of the two shadows. Beyond the
// shadow map, where the game has no shadow, the rays take it as letting half the sun through.
//
// Where it deviates from the denoiser's sample, and why:
// - The trace writes the denoiser's 8x4 tile masks itself, through groupshared memory, which works
//   at any wave size.
// - Motion comes from the camera, and from the game's velocity where a moving mesh drew
//   (gi_shadow.hlsli, ReadVelocity).
// - The temporal pass's neighbourhood and history count only pixels on the pixel's own surface
//   (ClassifyShadowTiles).
#include "gi_shadow.hlsli"
// The denoiser as AMD wrote it; its bitfield helpers shift an untyped literal.
#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"
#include "ffx_denoiser_shadows/ffx_denoiser_shadows_tileclassification.h"
#pragma dxc diagnostic pop

// How far a point lies beyond the game's shadow map: 0 inside it, 1 past its fade, as
// SunLight_DL_CS_ES fades its shadow to lit (sun_light_pass_shaders.hpp, SunShadow). In view space the
// fade runs from the point (0, 0, c8.x); its distance from there follows from the distance to the
// eye and the view depth.
float BeyondShadowMap(float3 position, float depth)
{
    if (g_Frame.shadowRange.w == 0.0f) return 1.0f;

    float centre = g_Frame.shadowRange.x;
    float viewDepth = toLinearDepth(depth, g_Frame.nearFar.xy);
    float toEye = distance(position, g_Frame.eye.xyz);
    float fromCentre = sqrt(max(toEye * toEye - 2.0f * viewDepth * centre + centre * centre, 0.0f));
    float fade = saturate(max(fromCentre - g_Frame.shadowRange.y, 0.0f) * g_Frame.shadowRange.z) * (viewDepth >= centre ? 1.0f : 0.0f);
    return fade * fade;
}

groupshared uint g_TileMask;

// One 8x4 tile a group: a ray a pixel, into the tile's mask. The sky and pixels past the edge count
// as lit, so they do not keep the denoiser from skipping a lit tile.
[numthreads(8, 4, 1)]
void TraceSunShadows(uint2 gtid : SV_GroupThreadID, uint2 gid : SV_GroupID, uint index : SV_GroupIndex)
{
    if (index == 0) g_TileMask = 0;
    GroupMemoryBarrierWithGroupSync();

    uint2 did = gid * uint2(8, 4) + gtid;
    bool lit = true;
    if (all(did < g_Frame.dimensions.xy))
    {
        float depth = g_Depth.Load(int3(did, 0));
        if (depth > 0.0f)
        {
            float3 position = DepthPosition(did, depth);
            float3 normal = DecodeNormal(g_Normal.Load(int3(did, 0)));
            Random random = MakeRandom(g_Frame.dimensions.z, did.y * g_Frame.dimensions.x + did.x);
            float3 toSun = SunSample(random.rand2());
            float3 origin, facing;
            float tolerance;
            ShadowRayStart(ShadowSurfaceAt(position, normal), toSun, origin, facing, tolerance);
            float blocking = BeyondShadowMap(position, depth) * g_Frame.shadowParams.y;
            // Rays to the sun run parallel: each keeps its pixel's footprint. The sun's size comes from
            // spreading the rays over its disc; a cone widened by it as well took cut-out alpha from
            // coarse levels of detail, where a pylon's thin bars fell under the threshold.
            float blocker, transmittance;
            bool blocked = SurfaceRayBlocked(origin, facing, tolerance, toSun, g_Frame.shadowParams.z, PixelCone(position, 0.0f), true, blocking, ShadowRayTowards(RayTowardsSun),
                random, blocker, transmittance);
            // The denoiser takes a pixel as lit or not: past hair, lit with the chance its light gets through.
            lit = !blocked && random.rand() < transmittance;
        }

        // DLSS Ray Reconstruction denoises the image: the sun pass reads the ray as it is.
        if (g_Frame.reconstruction.x != 0u) g_Shadow_Output[did] = lit ? 1.0f : 0.0f;
    }

    if (lit) InterlockedOr(g_TileMask, FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(did));
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) g_Shadow_Masks[FFX_DNSR_Shadows_LinearTileIndex(gid, g_Frame.dimensions.x)] = g_TileMask;
}

// ---- The denoiser's temporal pass ----
// AMD's FFX_DNSR_Shadows_TileClassification with two changes, both for what lies across an edge:
// - The local neighbourhood that bounds a pixel's history counts only the rays of pixels on its own
//   surface. AMD's counts every pixel within 8, whatever lies there: a sunlit character's rays lifted the
//   history of the shaded wall behind him, and its variance let the filters blur his light onto it, a lit
//   band along his outline 8 render pixels wide at any render scale (Mother Base, 2026-10-29).
// - The history comes from the last frame's pixels on the pixel's own surface (ReprojectFromSurface).

static const int NeighbourhoodRadius = KERNEL_RADIUS;
static const int NeighbourhoodSize = 8 + 2 * NeighbourhoodRadius; // The tile and the kernel's reach around it

groupshared float4 g_Neighbours[NeighbourhoodSize][NeighbourhoodSize]; // World position, and 1 where the ray reached the sun, 0 where not, -1 for none
groupshared float3 g_NeighbourNormals[NeighbourhoodSize][NeighbourhoodSize];

void LoadNeighbourhood(uint index, uint2 gid)
{
    for (uint i = index; i < NeighbourhoodSize * NeighbourhoodSize; i += 64)
    {
        uint2 at = uint2(i % NeighbourhoodSize, i / NeighbourhoodSize);
        int2 pixel = int2(gid * 8 + at) - NeighbourhoodRadius;
        float4 neighbour = float4(0.0f, 0.0f, 0.0f, -1.0f);
        float3 normal = 0.0f;
        if (FFX_DNSR_Shadows_IsShadowReciever(uint2(pixel)))
        {
            uint tile = FFX_DNSR_Shadows_LinearTileIndex(FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(pixel), g_Frame.dimensions.x);
            bool lit = (g_Shadow_Masks[tile] & FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(pixel)) != 0;
            neighbour = float4(DepthPosition(pixel, g_Depth.Load(int3(pixel, 0))), lit ? 1.0f : 0.0f);
            normal = DecodeNormal(g_Normal.Load(int3(pixel, 0)));
        }

        g_Neighbours[at.y][at.x] = neighbour;
        g_NeighbourNormals[at.y][at.x] = normal;
    }

    GroupMemoryBarrierWithGroupSync();
}

// AMD's 17x17 Gaussian over the rays, among the neighbours on the pixel's surface: each within 5% of the
// pixel's distance from the other's plane. Every pixel holds 0 or 1, so the mean is the second moment too.
// Also how many rays the mean is worth, the Gaussian's weights taken into account.
float SurfaceNeighbourhood(uint2 gtid, out float effectiveSamples)
{
    int2 centre = int2(gtid) + NeighbourhoodRadius;
    float3 position = g_Neighbours[centre.y][centre.x].xyz;
    float3 normal = g_NeighbourNormals[centre.y][centre.x];
    float tolerance = 0.05f * distance(position, g_Frame.eye.xyz);

    float lit = 0.0f, total = 0.0f, squares = 0.0f;
    for (int y = -NeighbourhoodRadius; y <= NeighbourhoodRadius; ++y)
    {
        for (int x = -NeighbourhoodRadius; x <= NeighbourhoodRadius; ++x)
        {
            float4 neighbour = g_Neighbours[centre.y + y][centre.x + x];
            float3 apart = neighbour.xyz - position;
            bool sameSurface = neighbour.w >= 0.0f && abs(dot(normal, apart)) <= tolerance
                && abs(dot(g_NeighbourNormals[centre.y + y][centre.x + x], apart)) <= tolerance;
            if (!sameSurface) continue;

            float weight = FFX_DNSR_Shadows_KernelWeight(abs(x)) * FFX_DNSR_Shadows_KernelWeight(abs(y));
            lit += weight * neighbour.w;
            total += weight;
            squares += weight * weight;
        }
    }

    effectiveSamples = squares > 0.0f ? total * total / squares : 1.0f;
    return total > 0.0f ? lit / total : 0.0f;
}

// The history and moments from the last frame's 2x2 pixels around where this one was, bilinear among
// those that pass AMD's depth test (FFX_DNSR_Shadows_IsDisoccluded) each; false when none does. AMD's
// tests the one pixel the point falls in, which with the upscaler's jitter often lay across an edge:
// along every edge the history started over each frame and the raw rays flickered (2026-10-29, gone
// with Jitter off).
bool ReprojectFromSurface(uint2 did, float depth, float2 historyUv, out float history, out float3 moments)
{
    history = 0.0f;
    moments = 0.0f;
    float2 uv = (did + 0.5f) * FFX_DNSR_Shadows_GetInvBufferDimensions();
    float2 ndc = (2.0f * uv - 1.0f) * float2(1.0f, -1.0f);
    float4 clip = mul(FFX_DNSR_Shadows_GetReprojectionMatrix(), float4(ndc, depth, 1.0f));
    float linearDepth = FFX_DNSR_Shadows_GetLinearDepth(did, clip.z / clip.w);
    float3 position = DepthPosition(did, depth);
    float alignment = pow(1.0f - dot(normalize(g_Frame.eye.xyz - position), FFX_DNSR_Shadows_ReadNormals(did)), 8);
    float tolerance = lerp(1e-2f, 1e-1f, alignment);

    float2 at = historyUv * ShadowSize() - 0.5f;
    int2 corner = int2(floor(at));
    float2 fraction = at - corner;
    float total = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1, i >> 1);
        int2 tap = corner + offset;
        if (any(tap < 0) || any(tap >= ShadowSize())) continue;

        float previousDepth = FFX_DNSR_Shadows_GetLinearDepth(tap, g_Shadow_Depths[1][tap]);
        if (abs(previousDepth - linearDepth) / linearDepth >= tolerance) continue;

        float2 axes = lerp(1.0f - fraction, fraction, float2(offset));
        float weight = axes.x * axes.y;
        history += weight * g_Shadow_History[tap].x;
        moments += weight * g_Shadow_Moments[1][tap];
        total += weight;
    }

    if (total < 0.01f)
    {
        moments = 0.0f;
        return false;
    }

    history /= total;
    moments /= total;
    return true;
}

// An 8x8 tile a group. It also keeps this frame's depth for the next.
[numthreads(64, 1, 1)]
void ClassifyShadowTiles(uint index : SV_GroupIndex, uint2 gid : SV_GroupID)
{
    uint2 gtid = FFX_DNSR_Shadows_RemapLane8x8(index); // A 2x2 region a quad, for QuadReadAcross
    uint2 did = gid * 8 + gtid;
    if (all(did < g_Frame.dimensions.xy)) g_Shadow_Depths[0][did] = g_Depth.Load(int3(did, 0));

    bool isShadowReceiver = FFX_DNSR_Shadows_IsShadowReciever(did);
    if (FFX_DNSR_Shadows_ThreadGroupAllTrue(!isShadowReceiver))
    {
        FFX_DNSR_Shadows_ClearTargets(did, gtid, gid, 0.0f, isShadowReceiver, false);
        return;
    }

    // Tiles whose rays all reached the sun, or none, around as far as the filters reach, need no denoising.
    bool allInLight, allInShadow;
    FFX_DNSR_Shadows_SearchSpatialRegion(gid, allInLight, allInShadow);
    if (FFX_DNSR_Shadows_ThreadGroupAllTrue(allInLight || allInShadow))
    {
        FFX_DNSR_Shadows_ClearTargets(did, gtid, gid, allInLight ? 1.0f : 0.0f, isShadowReceiver, allInLight);
        return;
    }

    FFX_DNSR_Shadows_WriteTileMetaData(gid, gtid, false, false);

    float depth = FFX_DNSR_Shadows_ReadDepth(did);
    float2 velocity = FFX_DNSR_Shadows_GetClosestVelocity(did, depth); // While every lane runs
    LoadNeighbourhood(index, gid);
    float effectiveSamples;
    float mean = SurfaceNeighbourhood(gtid, effectiveSamples);
    if (!isShadowReceiver)
    {
        FFX_DNSR_Shadows_WriteReprojectionResults(did, 0.0f);
        FFX_DNSR_Shadows_WriteMoments(did, 0.0f);
        return;
    }

    uint tile = FFX_DNSR_Shadows_LinearTileIndex(FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(did), g_Frame.dimensions.x);
    float current = (g_Shadow_Masks[tile] & FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(did)) ? 1.0f : 0.0f;

    // This pixel's moments over time, unless what it showed last frame lay elsewhere.
    float2 historyUv = (did + 0.5f) * FFX_DNSR_Shadows_GetInvBufferDimensions() - velocity;
    float surfaceHistory;
    float3 previousMoments;
    bool hasHistory = ReprojectFromSurface(did, depth, historyUv, surfaceHistory, previousMoments);
    float samples = previousMoments.z + 1.0f;
    float newMean = previousMoments.x + (current - previousMoments.x) / samples;
    float newSquares = previousMoments.y + (current - previousMoments.x) * (current - newMean);
    float variance = samples > 1.0f ? newSquares / (samples - 1.0f) : 1.0f;
    float3 moments = float3(newMean, newSquares, samples);

    // The history, clamped to the neighbourhood; the further off, the fewer of its samples count. The
    // neighbourhood's spread has the most its mean can be off by chance added (p (1 - p) / n at its
    // worst): the few rays of a thin edge's surface often agree, and a spread of 0 threw its history away.
    float spatialVariance = max(mean - mean * mean, 0.0f) + 0.25f / effectiveSamples;
    float deviation = sqrt(spatialVariance);
    float previous = FFX_DNSR_Shadows_IsFirstFrame() ? current : hasHistory ? surfaceHistory : FFX_DNSR_Shadows_ReadHistory(historyUv);
    float clamped = clamp(previous, mean - 0.5f * deviation, mean + 0.5f * deviation);
    float discontinuity = (previous - mean) / max(0.5f * deviation, 0.001f);
    moments.z *= exp(-discontinuity * discontinuity / 20.0f);
    if (moments.z < 16.0f) variance = max(variance, spatialVariance) * max(16.0f - moments.z, 1.0f);

    float historyWeight = sqrt(max(8.0f - moments.z, 0.0f) / 8.0f);
    FFX_DNSR_Shadows_WriteReprojectionResults(did, float2(lerp(clamped, current, lerp(0.05f, 1.0f, historyWeight)), variance));
    FFX_DNSR_Shadows_WriteMoments(did, moments);
}

// ---- Diagnostics: the shadow debug view and probe ----
// The sun ray from a pixel as the shadow rays trace it, but towards the sun's centre, without chance and
// stopping at hair, so the view shows it; and the whole ray past copies of the surface, cut-outs without
// their texture letting it through.

RWStructuredBuffer<GIShadowProbe> g_ShadowProbe : register(u77);
RWTexture2D<float4> g_ShadowDebug : register(u78); // The debug view, blended over the image

struct DebugRay
{
    ShadowSurface surface; // The surface it leaves
    float3 origin, normal, direction;
    float tolerance, nearEnd, beyond;
    uint copies;      // Copies of the surface skipped in the near segment
    SceneHit nearHit; // The shadow rays' near segment, past copies
    SceneHit farHit;  // The shadow rays' far segment; beyond the shadow map, cut-out geometry without the game's alpha as opaque
    SceneHit anyHit;  // The whole ray, past copies near the surface; cut-outs without their texture let it through
};

DebugRay TraceDebugRay(float3 position, float3 normal, float depth)
{
    DebugRay r;
    r.direction = g_Frame.sunDirection.xyz;
    r.surface = ShadowSurfaceAt(position, normal);
    ShadowRayStart(r.surface, r.direction, r.origin, r.normal, r.tolerance);
    r.beyond = BeyondShadowMap(position, depth);
    float rayLength = g_Frame.shadowParams.z;
    r.nearEnd = NearEnd(r.normal, r.tolerance, r.direction, rayLength);
    r.nearHit = TraceFromSurface(r.origin, r.normal, r.tolerance, r.direction, r.nearEnd, GI_MASK_SCENE, true, PixelCone(position, 0.0f), ShadowRayTowards(RayTowardsSun), r.copies);

    // Beyond the shadow map the shadow rays take cut-out geometry without the game's alpha as blocking
    // with chance: here always. Inside, they let the sun through it.
    r.farHit = TraceClosest(r.origin, r.direction, r.nearEnd, rayLength, GI_MASK_SCENE, true, r.beyond > 0.0f, PixelCone(position, 0.0f));
    uint wholeRayCopies;
    r.anyHit = TraceFromSurface(r.origin, r.normal, r.tolerance, r.direction, rayLength, GI_MASK_SCENE, true, PixelCone(position, 0.0f), ShadowRayTowards(RayTowardsSun),
        wholeRayCopies);
    return r;
}

// The debug view of where a pixel's shadow rays leave (ShadowSurfaceAt), against the depth's point and
// the G-buffer's normal. Grey: from the depth's point, the scene having no surface near it, or one facing
// over 60 degrees away from the game's normal (not the surface the game drew). Otherwise red where it lies
// nearer the eye than the depth's point, cyan where farther, clear up to 5 mm apart and full from 3 cm.
float4 ShadowStartColor(float3 position, float3 normal)
{
    ShadowSurface surface = ShadowSurfaceAt(position, normal);
    if (!surface.exact) return float4(0.5f, 0.5f, 0.5f, 0.6f);

    float apart = distance(surface.position, g_Frame.eye.xyz) - distance(position, g_Frame.eye.xyz);
    float strength = saturate((abs(apart) - 0.005f) / 0.025f);
    return float4(apart < 0.0f ? float3(1.0f, 0.1f, 0.1f) : float3(0.0f, 1.0f, 1.0f), 0.75f * strength);
}

// The debug view: what blocks each pixel's sun ray. Lavender: one of the game's models or characters;
// brown: the terrain; yellow: cut-out geometry; olive: cut-out geometry without its texture, which the
// rays let the sun through inside the shadow map; magenta: anything else only the whole ray finds.
// Clear where the sun reaches. With shadowDiagnostics.y, where the shadow rays leave instead
// (ShadowStartColor). The upscaler's composite marks the probe's pixel with a cross.
[numthreads(8, 8, 1)]
void DebugShadows(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    float depth = g_Depth.Load(int3(did, 0));
    float4 color = 0.0f;
    if (depth > 0.0f)
    {
        float3 position = DepthPosition(did, depth);
        float3 normal = DecodeNormal(g_Normal.Load(int3(did, 0)));
        if (g_Frame.shadowDiagnostics.y)
        {
            g_ShadowDebug[did] = ShadowStartColor(position, normal);
            return;
        }

        DebugRay r = TraceDebugRay(position, normal, depth);
        SceneHit real = r.farHit;
        if (r.nearHit.hit) real = r.nearHit;
        if (real.hit)
        {
            uint kind = InstanceKind(g_Instances[real.instance]);
            SceneTriangle t = LoadTriangle(real.instance, real.geometry, real.primitive);
            if (t.geometry.flags & GI_GEOMETRY_CUT_OUT) color = float4(1.0f, 0.9f, 0.0f, 0.6f);
            else if (kind & GI_INSTANCE_TERRAIN) color = float4(0.75f, 0.5f, 0.25f, 0.6f);
            else color = float4(0.55f, 0.45f, 1.0f, 0.6f);
        }
        else if (r.anyHit.hit)
        {
            SceneTriangle t = LoadTriangle(r.anyHit.instance, r.anyHit.geometry, r.anyHit.primitive);
            color = (t.geometry.flags & GI_GEOMETRY_CUT_OUT) ? float4(0.5f, 0.45f, 0.0f, 0.6f) : float4(1.0f, 0.0f, 1.0f, 0.75f);
        }
    }

    g_ShadowDebug[did] = color;
}

groupshared float3 g_ProbeReceiver, g_ProbeNormal;
groupshared uint g_ProbeValid;
groupshared uint4 g_ProbeLightHits[GI_PROBE_LIGHT_RAYS];

// The probe, one group: the centre pixel's sun ray, then its global illumination rays. Needs this
// frame's shadow passes done.
[numthreads(64, 1, 1)]
void ProbeShadows(uint index : SV_GroupIndex)
{
    if (index == 0)
    {
        g_ProbeValid = 0;

        GIShadowProbe probe = (GIShadowProbe)0;
        probe.terrain.w = float(g_Frame.dimensions.z);
        uint2 centre = g_Frame.dimensions.xy / 2;
        float depth = g_Depth.Load(int3(centre, 0));
        probe.receiver.w = depth;
        if (depth > 0.0f)
        {
            float3 position = DepthPosition(centre, depth);
            float3 normal = DecodeNormal(g_Normal.Load(int3(centre, 0)));
            DebugRay r = TraceDebugRay(position, normal, depth);
            probe.receiver.xyz = position;
            probe.normal = float4(normal, distance(position, g_Frame.eye.xyz));
            probe.ray = float4(r.direction, r.beyond);
            probe.segments = float4(r.nearEnd, r.tolerance, r.nearHit.hit ? r.nearHit.distance : -1.0f, r.farHit.hit ? r.farHit.distance : -1.0f);
            probe.hits = uint4(r.copies, r.nearHit.hit ? r.nearHit.instance + 1 : 0, r.farHit.hit ? r.farHit.instance + 1 : 0,
                r.anyHit.hit ? r.anyHit.instance + 1 : 0);
            if (r.anyHit.hit)
            {
                SceneTriangle t = LoadTriangle(r.anyHit.instance, r.anyHit.geometry, r.anyHit.primitive);
                uint kind = InstanceKind(g_Instances[r.anyHit.instance]);
                probe.hitGeometry = uint4(t.row, kind | (t.geometry.flags << 16), r.anyHit.geometry | (r.anyHit.frontFace ? 1u << 31 : 0u),
                    asuint(r.anyHit.distance));
                if (kind & GI_INSTANCE_TERRAIN)
                {
                    float3 corner = t.v0.y >= t.v1.y && t.v0.y >= t.v2.y ? t.v0 : t.v1.y >= t.v2.y ? t.v1 : t.v2;
                    probe.terrain.xyz = float3(corner.y, TerrainPoint(corner.xz).y, TerrainTile(TerrainUv(corner.xz)).z);
                }
            }

            // Where the rays start: the surface they leave, which side, and whatever the lift off it passes
            // through on the way (back from the start, stopping short of the surface itself).
            float3 lift = r.origin - r.surface.position;
            float liftLength = length(lift);
            probe.start = float4(r.surface.normal, r.surface.exact ? 1.0f : 0.0f);
            probe.lift = float4(lift, dot(r.normal, r.surface.normal) < 0.0f ? 1.0f : 0.0f);
            if (r.surface.exact)
                probe.startHit = uint4(r.surface.hit.instance + 1, r.surface.hit.geometry, r.surface.hit.primitive, r.anyHit.primitive);
            if (liftLength > 0.0f)
                probe.crossed = ProbeHit(TraceClosest(r.origin, -lift / liftLength, 0.0f, 0.95f * liftLength, GI_MASK_SCENE, true, false,
                    PixelCone(position, 0.0f)));

            uint tile = FFX_DNSR_Shadows_LinearTileIndex(FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(centre), g_Frame.dimensions.x);
            bool rayLit = (g_Shadow_Masks[tile] & FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(centre)) != 0;
            probe.shadow = float4(g_Shadow_Output[centre], rayLit ? 1.0f : 0.0f, 0.0f, 0.0f);
            // The camera's ray through the centre pixel, as the scene view traces it.
            float3 farPoint = DepthPosition(centre, 1e-6f);
            probe.view = ProbeHit(TraceClosest(g_Frame.eye.xyz, normalize(farPoint - g_Frame.eye.xyz), 1e5f));
            g_ProbeReceiver = position;
            g_ProbeNormal = normal;
            g_ProbeValid = 1;
        }

        g_ShadowProbe[0] = probe;
    }

    GroupMemoryBarrierWithGroupSync();

    // The centre pixel's global illumination rays, as the probes trace theirs, spread evenly.
    if (index < GI_PROBE_LIGHT_RAYS)
    {
        uint4 lightHit = 0u;
        if (g_ProbeValid)
        {
            float2 spread = float2((index + 0.5f) / GI_PROBE_LIGHT_RAYS, frac(index * 0.618034f));
            float3 direction = mapToCosineHemisphere(spread, g_ProbeNormal);
            float3 origin = OffsetFromTriangle(g_ProbeReceiver, g_ProbeNormal);
            lightHit = ProbeHit(TraceFromSurface(origin, g_ProbeNormal, CopyTolerance(g_ProbeReceiver), direction, 1e9f));
        }

        g_ProbeLightHits[index] = lightHit;
    }

    GroupMemoryBarrierWithGroupSync();
    if (index == 0)
    {
        uint4 lightRays = 0u, nearest = 0u;
        for (uint ray = 0; ray < GI_PROBE_LIGHT_RAYS; ++ray)
        {
            uint4 hit = g_ProbeLightHits[ray];
            if (!hit.x)
            {
                ++lightRays.z;
                continue;
            }

            float hitDistance = asfloat(hit.w);
            lightRays += uint4(hitDistance < 1.0f, hitDistance < 10.0f, 0, (hit.y >> 31) == 0);
            if (!nearest.x || hitDistance < asfloat(nearest.w)) nearest = hit;
        }

        g_ShadowProbe[0].lightRays = lightRays;
        g_ShadowProbe[0].lightNearest = nearest;
    }
}
