// Where a ray meets the scene, lit as the global illumination's rays light their hits (docs/DEVELOPMENT.md,
// "Global illumination"), and the hash grid cache their bounces end at: AMD's two-level hash grid of GI-1.2
// (Capsaicin, MIT; licenses/Capsaicin-LICENSE.txt). For the global illumination's light (gi_light.hlsl) and
// path tracing's reflections (reflections.hlsl), which read the cache as it stands.
#ifndef GI_HIT_LIGHT_HLSLI
#define GI_HIT_LIGHT_HLSLI

#include "ggx_specular.hlsli"
#include "gi_common.hlsli"

// ---- Bindings, after gi_common.hlsli's ----

RWStructuredBuffer<uint> g_HashGridCache_HashBuffer : register(u16);
RWStructuredBuffer<uint> g_HashGridCache_DecayTileBuffer : register(u17);
RWStructuredBuffer<uint2> g_HashGridCache_ValueBuffer : register(u18);
RWStructuredBuffer<uint2> g_HashGridCache_ValueIndirectBuffer : register(u19);
RWStructuredBuffer<uint> g_HashGridCache_UpdateTileBuffer : register(u20);
RWStructuredBuffer<uint> g_HashGridCache_UpdateCellValueBuffer : register(u21);
RWStructuredBuffer<uint> g_HashGridCache_UpdateCellValueIndirectBuffer : register(u22);
RWStructuredBuffer<float4> g_HashGridCache_VisibilityBuffer : register(u23);
RWStructuredBuffer<uint> g_HashGridCache_VisibilityCellBuffer : register(u24);
RWStructuredBuffer<uint> g_HashGridCache_VisibilityQueryBuffer : register(u25);
RWStructuredBuffer<uint2> g_HashGridCache_MultibounceInfoBuffer : register(u26); // The second hit's albedo
RWStructuredBuffer<uint> g_HashGridCache_PackedTileIndexBuffer : register(u27);
RWStructuredBuffer<uint> g_HashGridCache_PreviousPackedTileIndexBuffer : register(u28);
RWStructuredBuffer<uint> g_HashGridCache_LightBuffer : register(u29); // Hits to light: their visibility index

#define kGI1_InvalidId 0xFFFFFFFFu
#define kHashGridCache_TileDecay 50
#define kHashGridCache_FloatQuantize 1e3f
#define MAX_HIT_DISTANCE 1e9f

// Capsaicin's names for this frame's constants.
#define g_BufferDimensions g_Frame.dimensions.xy
#define g_FrameIndex g_Frame.dimensions.z
#define g_Eye g_Frame.eye.xyz
#define g_NearFar g_Frame.nearFar.xy
#define g_ViewProjection g_Frame.viewProjection
#define HASH_CELL_SIZE g_Frame.cellSizes.y
#define HASH_MIN_CELL_SIZE g_Frame.cellSizes.z
#define HASH_NUM_BUCKETS g_Frame.hashCounts.x
#define HASH_NUM_TILES_PER_BUCKET g_Frame.hashCounts.w
#define HASH_SIZE_TILE_MIP0 g_Frame.hashTileSizes.x
#define HASH_NUM_CELLS_PER_TILE_MIP0 g_Frame.hashTileCells.x
#define HASH_NUM_CELLS_PER_TILE g_Frame.hashMisc.x
#define HASH_TILE_CELL_RATIO g_Frame.hashSamples.w
#define HASH_MAX_SAMPLE_COUNT g_Frame.hashSamples.x
#define HASH_DISCARD_MULTIBOUNCE g_Frame.hashSamples.y
#define HASH_MAX_MULTIBOUNCE_SAMPLE_COUNT g_Frame.hashSamples.z
#define HASH_CELL_SAMPLES_PER_FRAME g_Frame.hashMisc.z
#define PACKED_TILE_COUNT (GI_COUNTER_PACKED_TILES + g_Frame.hashMisc.y)
#define PREVIOUS_PACKED_TILE_COUNT (GI_COUNTER_PACKED_TILES + 1 - g_Frame.hashMisc.y)

// ---- Hash grid cache (Capsaicin's hash_grid_cache.hlsl) ----

// A hit kept for later passes, with the ray's direction, which turns the triangle's normal towards
// where the ray came from.
struct HashGridCache_Visibility
{
    float3 direction;
    uint instanceIndex;
    uint geometryIndex; // Within the instance's structure
    uint primitiveIndex;
    float2 barycentrics;
};

HashGridCache_Visibility VisibilityOf(SceneHit hit, float3 direction)
{
    HashGridCache_Visibility visibility;
    visibility.direction = direction;
    visibility.instanceIndex = hit.instance;
    visibility.geometryIndex = hit.geometry;
    visibility.primitiveIndex = hit.primitive;
    visibility.barycentrics = hit.barycentrics;
    return visibility;
}

#define HASHGRIDCACHE_STEP_FACTOR 1e3f
#define HASHGRIDCACHE_SIZE_FACTOR 1e-3f

struct HashGridCache_Desc
{
    uint bucketIndex;
    uint tileHash;
    uint2 cellOffset;
};

HashGridCache_Desc HashGridCache_GetDesc(float3 hitPosition, float3 direction, float hitDistance)
{
    float step = max(distance(g_Eye, hitPosition) * HASH_CELL_SIZE, HASH_MIN_CELL_SIZE);
    float logStep = floor(log2(HASHGRIDCACHE_STEP_FACTOR * step));
    float hitCellSize = HASHGRIDCACHE_SIZE_FACTOR * exp2(logStep);
    float hitTileSize = hitCellSize * HASH_TILE_CELL_RATIO;

    float3 signedC = floor(hitPosition / hitTileSize);
    float3 absDirection = abs(direction);
    float maxDirection = max(max(absDirection.x, absDirection.y), absDirection.z);
    uint axis = absDirection.x == maxDirection ? 0 : absDirection.y == maxDirection ? 1 : 2;

    uint l = uint(logStep);
    uint3 c = asuint(int3(signedC));
    uint3 d = uint3(axis, direction[axis] < 0.0f ? 1 : 0, 0);
    uint t = uint(hitDistance < hitTileSize);

    HashGridCache_Desc desc;
    desc.bucketIndex = pcgHash(l + pcgHash(c.x + pcgHash(c.y + pcgHash(c.z + pcgHash(d.x + pcgHash(d.y + pcgHash(d.z + pcgHash(t))))))))
        % HASH_NUM_BUCKETS;
    desc.tileHash = max(1, xxHash(l + xxHash(c.x + xxHash(c.y + xxHash(c.z + xxHash(d.x + xxHash(d.y + xxHash(d.z + xxHash(t)))))))));

    float3 e = floor(hitPosition / hitCellSize) - floor(hitPosition / hitTileSize) * HASH_TILE_CELL_RATIO;
    if (absDirection.x == maxDirection) desc.cellOffset = uint2(e.y, e.z);
    else if (absDirection.y == maxDirection) desc.cellOffset = uint2(e.x, e.z);
    else desc.cellOffset = uint2(e.x, e.y);

    return desc;
}

uint HashGridCache_CellOffsetMip0(uint cellIndexMip0, out uint2 cellOffsetMip0)
{
    uint tileIndex = cellIndexMip0 / HASH_NUM_CELLS_PER_TILE;
    uint linearOffset = cellIndexMip0 % HASH_NUM_CELLS_PER_TILE;
    cellOffsetMip0.y = linearOffset / HASH_SIZE_TILE_MIP0;
    cellOffsetMip0.x = linearOffset - cellOffsetMip0.y * HASH_SIZE_TILE_MIP0;
    return tileIndex;
}

uint HashGridCache_CellIndex(uint2 cellOffsetMip0, uint tileIndex, uint mip)
{
    uint mipSize = HASH_SIZE_TILE_MIP0 >> mip;
    uint2 cellOffset = cellOffsetMip0 >> mip;
    return tileIndex * HASH_NUM_CELLS_PER_TILE + g_Frame.hashTileFirstCells[mip] + cellOffset.x + cellOffset.y * mipSize;
}

uint HashGridCache_CellIndex(uint linearOffset, uint tileIndex)
{
    return tileIndex * HASH_NUM_CELLS_PER_TILE + linearOffset;
}

uint HashGridCache_InsertCell(float3 hitPosition, float3 direction, float hitDistance, out uint tileIndex, out bool isNewTile)
{
    uint bucketOffset;
    isNewTile = false;
    tileIndex = kGI1_InvalidId;
    HashGridCache_Desc desc = HashGridCache_GetDesc(hitPosition, direction, hitDistance);
    for (bucketOffset = 0; bucketOffset < HASH_NUM_TILES_PER_BUCKET; ++bucketOffset)
    {
        uint previousHash;
        tileIndex = bucketOffset + desc.bucketIndex * HASH_NUM_TILES_PER_BUCKET;
        InterlockedCompareExchange(g_HashGridCache_HashBuffer[tileIndex], 0, desc.tileHash, previousHash);
        if (previousHash == 0)
        {
            isNewTile = true;
            break;
        }

        if (previousHash == desc.tileHash) break;
    }

    if (bucketOffset >= HASH_NUM_TILES_PER_BUCKET) return kGI1_InvalidId;

    return HashGridCache_CellIndex(desc.cellOffset, tileIndex, 0);
}

uint4 HashGridCache_QuantizeRadiance(float3 radiance)
{
    return uint4(uint(round(kHashGridCache_FloatQuantize * radiance.x)), uint(round(kHashGridCache_FloatQuantize * radiance.y)),
        uint(round(kHashGridCache_FloatQuantize * radiance.z)), 1);
}

float4 HashGridCache_RecoverRadiance(uint4 quantized)
{
    return float4(quantized.xyz / kHashGridCache_FloatQuantize, quantized.w);
}

// The four sums from `first` (rgb quantised, and the samples), zeroed for the next frame's adds.
uint4 TakeQuantizedRadiance(RWStructuredBuffer<uint> sums, uint first)
{
    uint4 taken = uint4(sums[first + 0], sums[first + 1], sums[first + 2], sums[first + 3]);
    sums[first + 0] = 0;
    sums[first + 1] = 0;
    sums[first + 2] = 0;
    sums[first + 3] = 0;
    return taken;
}

float4 HashGridCache_PackVisibility(HashGridCache_Visibility visibility)
{
    return float4(asfloat(visibility.instanceIndex | (visibility.geometryIndex << GI_HIT_INSTANCE_BITS)), asfloat(packNormal(visibility.direction)),
        asfloat(visibility.primitiveIndex),
        asfloat((f32tof16(visibility.barycentrics.x) << 16) | f32tof16(visibility.barycentrics.y)));
}

HashGridCache_Visibility HashGridCache_UnpackVisibility(float4 packed)
{
    HashGridCache_Visibility visibility;
    visibility.instanceIndex = asuint(packed.x) & ((1u << GI_HIT_INSTANCE_BITS) - 1);
    visibility.geometryIndex = asuint(packed.x) >> GI_HIT_INSTANCE_BITS;
    visibility.direction = unpackNormal(asuint(packed.y));
    visibility.primitiveIndex = asuint(packed.z);
    visibility.barycentrics.x = f16tof32(asuint(packed.w) >> 16);
    visibility.barycentrics.y = f16tof32(asuint(packed.w) & 0xFFFFu);
    return visibility;
}

// The filtered radiance of a cell: the first mip of its tile with enough samples.
float4 HashGridCache_FilteredRadiance(RWStructuredBuffer<uint2> values, uint cellIndexMip0, float enough)
{
    uint2 offsetMip0;
    uint tileIndex = HashGridCache_CellOffsetMip0(cellIndexMip0, offsetMip0);
    float4 radiance = unpackHalf4(values[cellIndexMip0]);
    for (uint mip = 1; mip < 4; ++mip)
    {
        if (radiance.w >= enough || g_Frame.hashTileSizes[mip] == 0) break;

        radiance = unpackHalf4(values[HashGridCache_CellIndex(offsetMip0, tileIndex, mip)]);
    }

    return radiance;
}

float3 HashGridCache_FilteredDirect(uint cellIndex)
{
    float4 radiance = HashGridCache_FilteredRadiance(g_HashGridCache_ValueBuffer, cellIndex, HASH_MAX_SAMPLE_COUNT);
    return radiance.rgb / max(radiance.w, 1.0f);
}

float3 HashGridCache_FilteredIndirect(uint cellIndex)
{
    float4 radiance = HashGridCache_FilteredRadiance(g_HashGridCache_ValueIndirectBuffer, cellIndex, HASH_MAX_MULTIBOUNCE_SAMPLE_COUNT);
    return radiance.rgb / max(radiance.w, 1.0f);
}

// Claims a cell for this frame: new tiles join the packed list with their cells cleared, and the
// first touch of a tile this frame queues it for UpdateTiles.
void HashGridCache_TouchCell(uint tileIndex, bool isNewTile)
{
    uint previousDecay;
    InterlockedExchange(g_HashGridCache_DecayTileBuffer[tileIndex], g_FrameIndex, previousDecay);

    if (isNewTile)
    {
        uint packedTileIndex;
        InterlockedAdd(g_Counters[PACKED_TILE_COUNT], 1, packedTileIndex);
        g_HashGridCache_PackedTileIndexBuffer[packedTileIndex] = tileIndex;
        for (uint offset = 0; offset < HASH_NUM_CELLS_PER_TILE_MIP0; ++offset)
        {
            uint cell = HashGridCache_CellIndex(offset, tileIndex);
            g_HashGridCache_ValueBuffer[cell] = uint2(0, 0);
            g_HashGridCache_ValueIndirectBuffer[cell] = uint2(0, 0);
        }
    }

    if (isNewTile || previousDecay != g_FrameIndex)
    {
        uint updateTileIndex;
        InterlockedAdd(g_Counters[GI_COUNTER_UPDATE_TILES], 1, updateTileIndex);
        g_HashGridCache_UpdateTileBuffer[updateTileIndex] = tileIndex;
    }
}

// Queues a hit for PopulateCells as one of its cell's samples while the cell has fewer than
// HASH_CELL_SAMPLES_PER_FRAME this frame (the claims count in its update's w).
void HashGridCache_QueueLight(uint visibilityIndex, uint cellIndex)
{
    uint claimed;
    InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 3], 1, claimed);
    if (claimed >= HASH_CELL_SAMPLES_PER_FRAME) return;

    uint index;
    InterlockedAdd(g_Counters[GI_COUNTER_LIGHTS], 1, index);
    g_HashGridCache_LightBuffer[index] = visibilityIndex;
}

// A hit the cache keeps, for PopulateCells and the multibounce: `query` is where it came from.
uint HashGridCache_Keep(uint counter, uint first, HashGridCache_Visibility visibility, uint cellIndex, uint query)
{
    uint index;
    InterlockedAdd(g_Counters[counter], 1, index);
    uint visibilityIndex = first + index;
    g_HashGridCache_VisibilityBuffer[visibilityIndex] = HashGridCache_PackVisibility(visibility);
    g_HashGridCache_VisibilityCellBuffer[visibilityIndex] = cellIndex;
    g_HashGridCache_VisibilityQueryBuffer[visibilityIndex] = query;
    return index;
}

// ---- Hits ----

// World position and geometric normal (facing where the ray came from) of a stored hit.
void HitSurface(HashGridCache_Visibility visibility, out float3 world, out float3 normal)
{
    SceneTriangle t = LoadTriangle(visibility.instanceIndex, visibility.geometryIndex, visibility.primitiveIndex);
    world = TrianglePoint(t, visibility.barycentrics);
    normal = FacingNormal(t, visibility.direction);
}

// The level of detail a hit reads a texture at: the ray cone's, `footprint` metres across there; or without one,
// a few texels' mean: the global illumination's light varies over centimetres to metres.
float HitLevel(Texture2D<float4> texture, SceneTriangle t, float footprint)
{
    uint width, height, levels;
    texture.GetDimensions(0, width, height, levels);
    if (footprint <= 0.0f) return max(float(levels) - 6.0f, 0.0f);

    float2 uv0 = VertexUV(t.geometry, t.indices.x), uv1 = VertexUV(t.geometry, t.indices.y), uv2 = VertexUV(t.geometry, t.indices.z);
    return clamp(TextureLod(t.v0, t.v1, t.v2, uv0, uv1, uv2, float2(width, height), footprint), 0.0f, float(levels) - 1.0f);
}

// A hit's albedo: the sea's water; none on a self-lit mesh, whose texture is its light; the G-buffer's where the
// camera sees the hit; otherwise its mesh's albedo texture, or its mesh's mean.
float3 HitAlbedo(float3 world, HashGridCache_Visibility visibility, float footprint)
{
    GIInstanceDesc instance = g_Instances[visibility.instanceIndex];
    if (InstanceKind(instance) & GI_INSTANCE_SEA) return float3(GI_SEA_ALBEDO);
    if (g_Geometries[FirstGeometry(instance) + visibility.geometryIndex].flags & GI_GEOMETRY_EMISSIVE) return 0.0f;

    float4 clip = mul(g_ViewProjection, float4(world, 1.0f));
    if (clip.w > 0.0f)
    {
        float2 ndc = clip.xy / clip.w;
        if (all(abs(ndc) < 1.0f))
        {
            uint2 pixel = min(uint2((ndc * float2(0.5f, -0.5f) + 0.5f) * g_BufferDimensions), g_BufferDimensions - 1);
            float depth = g_Depth.Load(int3(pixel, 0));
            if (depth > 0.0f && abs(toLinearDepth(depth, g_NearFar) - clip.w) < 0.02f * clip.w) return g_Albedo.Load(int3(pixel, 0)).rgb;
        }
    }

    SceneTriangle t = LoadTriangle(visibility.instanceIndex, visibility.geometryIndex, visibility.primitiveIndex);
    if (t.geometry.texture && (t.geometry.flags & GI_GEOMETRY_MASK) == 0)
    {
        Texture2D<float4> texture = g_SceneTextures[NonUniformResourceIndex(t.geometry.texture - 1)];
        return texture.SampleLevel(g_WrapSampler, TriangleUV(t.geometry, t.indices, visibility.barycentrics), HitLevel(texture, t, footprint)).rgb;
    }

    uint4 learnt = g_GeometryAlbedo[t.row];
    return learnt.w ? float3(learnt.xyz) / (255.0f * learnt.w) : g_Frame.nearFar.z;
}

float3 HitAlbedo(float3 world, HashGridCache_Visibility visibility)
{
    return HitAlbedo(world, visibility, 0.0f);
}

// A self-lit mesh's light where a ray meets it: its own, times its texture's colour, in the rays' units.
float3 EmissionAt(SceneTriangle t, float2 barycentrics)
{
    float3 light = t.geometry.emission.rgb * g_Frame.emission.x;
    if (!t.geometry.texture || !(t.geometry.flags & GI_GEOMETRY_UV)) return light;

    Texture2D<float4> texture = g_SceneTextures[NonUniformResourceIndex(t.geometry.texture - 1)];
    return light * texture.SampleLevel(g_WrapSampler, TriangleUV(t.geometry, t.indices, barycentrics), 0.0f).rgb;
}

// The surface's normal at a hit, as the G-buffer has it: its vertices' normals and its normal map. On the side the
// ray came from, and the triangle's own where a bent normal would face away from the ray.
float3 ShadingNormal(SceneTriangle t, HashGridCache_Visibility visibility, float3 geometric, float footprint)
{
    if (!t.geometry.normalOffset) return geometric;

    float2 b = visibility.barycentrics;
    float3 weights = float3(1.0f - b.x - b.y, b.x, b.y);
    float3 n0, n1, n2;
    float4 t0, t1, t2;
    VertexFrame(t.geometry, t.indices.x, n0, t0);
    VertexFrame(t.geometry, t.indices.y, n1, t1);
    VertexFrame(t.geometry, t.indices.z, n2, t2);

    GIInstanceDesc instance = g_Instances[visibility.instanceIndex];
    float3 normal = ToWorldDirection(instance, weights.x * n0 + weights.y * n1 + weights.z * n2);
    float3 tangent = ToWorldDirection(instance, weights.x * t0.xyz + weights.y * t1.xyz + weights.z * t2.xyz);
    if (!(dot(normal, normal) > 0.0f)) return geometric;

    normal = normalize(normal);
    if (t.geometry.normalTexture && (t.geometry.flags & GI_GEOMETRY_UV))
    {
        // fox3ddf_blin's decode: x in alpha, y in green, z what is left; its binormal is cross(normal, tangent) x w.
        Texture2D<float4> map = g_SceneTextures[NonUniformResourceIndex(t.geometry.normalTexture - 1)];
        float2 xy = map.SampleLevel(g_WrapSampler, TriangleUV(t.geometry, t.indices, b), HitLevel(map, t, footprint)).ag * 2.0f - 1.0f;
        tangent = tangent - normal * dot(tangent, normal);
        if (dot(tangent, tangent) > 0.0f)
        {
            tangent = normalize(tangent);
            normal = normalize(xy.x * tangent + xy.y * t0.w * cross(normal, tangent) + sqrt(saturate(1.0f - dot(xy, xy))) * normal);
        }
    }

    if (dot(normal, geometric) < 0.0f) normal = -normal;
    return dot(normal, -visibility.direction) > 0.0f ? normal : geometric;
}

// A hit as path tracing's reflections shade it (HitOwnLight); `isMatte`, a matte surface of its triangle's normal, all
// the global illumination's diffuse rays need: reading its material for them cost 2.5 ms and showed nothing
// (2026-10-30). `footprint`: the ray's cone there (HitLevel).
struct HitMaterial
{
    float3 world;
    float3 geometric;     // Its triangle's normal, towards where the ray came from
    float3 normal;        // What it is lit with
    float3 albedo;
    float3 specularColor; // At normal incidence, as the game's table and SRM give it
    float roughness;
    float3 emission;      // A self-lit mesh's light
    bool isSelfLit;
    bool isLeaf;
};

HitMaterial HitMaterialOf(HashGridCache_Visibility visibility, float footprint, bool isMatte)
{
    SceneTriangle t = LoadTriangle(visibility.instanceIndex, visibility.geometryIndex, visibility.primitiveIndex);
    HitMaterial material;
    material.world = TrianglePoint(t, visibility.barycentrics);
    material.geometric = FacingNormal(t, visibility.direction);
    material.normal = material.geometric;
    material.albedo = 0.0f;
    material.specularColor = 0.0f;
    material.roughness = 1.0f;
    material.emission = 0.0f;
    material.isSelfLit = (t.geometry.flags & GI_GEOMETRY_EMISSIVE) != 0;
    material.isLeaf = (t.geometry.flags & GI_GEOMETRY_LEAF) != 0;
    if (material.isSelfLit)
    {
        material.emission = EmissionAt(t, visibility.barycentrics);
        return material;
    }

    material.albedo = HitAlbedo(material.world, visibility, footprint);
    if (isMatte || !g_Frame.reconstruction.y) return material;

    // The game's sea has no SRM: water's own reflectance, as the guides give it (ray_tracing.cpp).
    if (InstanceKind(g_Instances[visibility.instanceIndex]) & GI_INSTANCE_SEA)
    {
        material.specularColor = 0.02f;
        material.roughness = 0.1f;
        return material;
    }

    material.normal = ShadingNormal(t, visibility, material.geometric, footprint);
    if (t.geometry.specularTexture && (t.geometry.flags & GI_GEOMETRY_UV))
    {
        // fox3ddf_blin's G-buffer: r the specular rate, g the roughness.
        Texture2D<float4> srm = g_SceneTextures[NonUniformResourceIndex(t.geometry.specularTexture - 1)];
        float2 rateAndRoughness = srm.SampleLevel(g_WrapSampler, TriangleUV(t.geometry, t.indices, visibility.barycentrics), HitLevel(srm, t, footprint)).rg;
        material.specularColor = rateAndRoughness.x * g_MaterialSpecular.Load(int3(t.geometry.materialIndex, 0, 0)).rgb;
        material.roughness = rateAndRoughness.y;
    }

    return material;
}

// Whether the rays towards self-lit meshes aim at this one (SampledEmitterLight).
bool IsAimedAt(uint instance, uint geometry)
{
    for (uint i = 0; i < g_Frame.emitterCount.x; ++i)
        if (g_Frame.emitters[i].place.x == instance && g_Frame.emitters[i].place.y == geometry) return true;

    return false;
}

// What a diffuse ray takes of a self-lit mesh it meets: one the rays towards self-lit meshes aim at gives its diffuse
// light that way, so only the others' light comes through the ray. The specular rays always take it: those
// rays are what brings a self-lit mesh into a reflection.
float3 DiffuseRayEmission(HashGridCache_Visibility visibility, float3 emission)
{
    if (!any(emission > 0.0f) || IsAimedAt(visibility.instanceIndex, visibility.geometryIndex)) return 0.0f;

    return min(emission, g_Frame.emission.y);
}

// Leaves (GI_GEOMETRY_LEAF) let light through.
bool HitIsLeaf(HashGridCache_Visibility visibility)
{
    return (g_Geometries[FirstGeometry(g_Instances[visibility.instanceIndex]) + visibility.geometryIndex].flags & GI_GEOMETRY_LEAF) != 0;
}

// The light at a hit from one of the frame's point and spot lights, chosen by its light here without
// shadows (weighted reservoir sampling, a candidate a light), through a shadow ray to it: its light before
// the cosine, over the chance it was chosen.
float3 SampledLocalLightReaching(float3 world, float3 normal, float3 origin, float tolerance, inout Random random, out float3 towardsLight)
{
    towardsLight = normal;
    float total = 0.0f, chosenWeight = 0.0f;
    float3 chosenLight = 0.0f, chosenDirection = 0.0f;
    float chosenDistance = 0.0f, chosenClearance = 0.0f;
    for (uint i = 0; i < g_Frame.localLightCount.x; ++i)
    {
        GILocalLight candidate = g_Frame.localLights[i];
        float3 toLight;
        float lightDistance;
        float3 candidateLight = LocalLightAt(candidate, world, normal, toLight, lightDistance);
        float weight = luminance(candidateLight);
        if (!(weight > 0.0f)) continue;

        total += weight;
        if (random.rand() * total < weight)
        {
            chosenWeight = weight;
            chosenLight = candidateLight;
            chosenDirection = toLight;
            chosenDistance = lightDistance;
            chosenClearance = ClearanceTowards(candidate, toLight);
        }
    }

    if (!(chosenWeight > 0.0f)) return 0.0f;

    towardsLight = chosenDirection;
    float rayLength = chosenDistance - chosenClearance;
    float visibility = rayLength > 0.0f ? VisibilityFromSurface(origin, normal, tolerance, chosenDirection, rayLength, ShadowRayTowards(RayTowardsLamp)) : 1.0f;
    return chosenLight * (total / (chosenWeight * dot(normal, chosenDirection)) * visibility);
}

// The same with the cosine: the diffuse light at the hit.
float3 SampledLocalLight(float3 world, float3 normal, float3 origin, float tolerance, inout Random random)
{
    float3 toLight;
    float3 reaching = SampledLocalLightReaching(world, normal, origin, tolerance, random, toLight);
    return reaching * saturate(dot(normal, toLight));
}

// The diffuse light at a surface from one of the self-lit meshes the frame aims at (GIFrameConstants::emitters): one
// chosen by its light here (weighted reservoir sampling), a triangle of it and a point on that, through a shadow ray:
// its radiance at the point, times the cosines at both ends and the triangle's area over the squared distance, in the
// game's diffuse light units (over pi), over the chances of what was chosen: an unbiased estimate of their diffuse
// light. Their specular light comes by the reflections' own rays meeting them.
float3 SampledEmitterLight(float3 world, float3 normal, float3 origin, float tolerance, inout Random random)
{
    float total = 0.0f, chosenWeight = 0.0f;
    uint chosen = 0u;
    for (uint i = 0; i < g_Frame.emitterCount.x; ++i)
    {
        float3 towardsCentre = g_Frame.emitters[i].sphere.xyz - world;
        float centreSquared = dot(towardsCentre, towardsCentre);
        float reach = g_Frame.emitters[i].light.w;
        if (centreSquared >= reach * reach) continue;

        // Above the surface's horizon, for as much of its sphere as may rise over it.
        float radius = g_Frame.emitters[i].sphere.w;
        float centreDistance = max(sqrt(centreSquared), 1e-3f);
        float above = dot(normal, towardsCentre) / centreDistance + saturate(radius / centreDistance);
        if (!(above > 0.0f)) continue;

        float weight = luminance(g_Frame.emitters[i].light.rgb) * saturate(above) / max(centreSquared, radius * radius);
        total += weight;
        if (random.rand() * total < weight)
        {
            chosen = i;
            chosenWeight = weight;
        }
    }

    if (!(chosenWeight > 0.0f)) return 0.0f;

    GIEmitter emitter = g_Frame.emitters[chosen];
    SceneTriangle t = LoadTriangle(emitter.place.x, emitter.place.y, random.randInt(emitter.place.z));
    float2 u = random.rand2();
    float root = sqrt(u.x);
    float2 barycentrics = float2(root * (1.0f - u.y), root * u.y);
    float3 offset = TrianglePoint(t, barycentrics) - world;
    float pointSquared = dot(offset, offset);
    float pointDistance = sqrt(pointSquared);
    float3 direction = offset / max(pointDistance, 1e-4f);
    float cosine = dot(normal, direction);
    float3 edges = cross(t.v1 - t.v0, t.v2 - t.v0);
    float twiceArea = length(edges);
    if (!(cosine > 0.0f) || !(twiceArea > 0.0f)) return 0.0f;

    // The side it shines from: its vertices' normal, which knows it; both of a mesh without.
    float cosineThere = abs(dot(edges, direction)) / twiceArea;
    if (t.geometry.normalOffset)
    {
        float3 n0, n1, n2;
        float4 unusedTangent;
        VertexFrame(t.geometry, t.indices.x, n0, unusedTangent);
        VertexFrame(t.geometry, t.indices.y, n1, unusedTangent);
        VertexFrame(t.geometry, t.indices.z, n2, unusedTangent);
        float3 facing = ToWorldDirection(g_Instances[emitter.place.x], (1.0f - barycentrics.x - barycentrics.y) * n0 + barycentrics.x * n1 + barycentrics.y * n2);
        if (dot(facing, direction) > 0.0f) return 0.0f;
    }

    float3 radiance = min(EmissionAt(t, barycentrics), g_Frame.emission.y);
    if (!any(radiance > 0.0f)) return 0.0f;

    float visibility = VisibilityFromSurface(origin, normal, tolerance, direction, max(pointDistance - 0.01f, 0.0f), ShadowRayTowards(RayTowardsLamp));
    float chances = emitter.place.z * total / chosenWeight;
    return radiance * (visibility * cosine * cosineThere * twiceArea * chances / (TWO_PI * max(pointSquared, 1e-3f)));
}

// The share of the light on a leaf's far side it lets through to the near side, over the share it reflects
// (its albedo): a leaf is a thin surface that reflects and lets through light diffusely, as pbrt's
// DiffuseTransmissionMaterial. Green leaves let through a half to two thirds as much light as they reflect
// across the visible range (leaf optics measurements, PROSPECT). Without it a bush lit only by what its
// leaves reflected was black inside: the rays from inside it all met leaves within a metre (the shadow
// probe, 2026-10-24), each lit on its outer side alone. The sun's shadow rays still stop at a leaf: the
// light it lets through is scattered, which its glow here carries to what is around it.
static const float LeafTransmittance = 0.5f;

// The sun as a disc of its angular size at a hit: a shadow ray to a point on it from the side it lights,
// a leaf's far side too. Its light before the cosine and the albedo.
float3 SunReaching(float3 world, float3 normal, float3 origin, float tolerance, bool leaf, inout Random random, out float3 direction)
{
    direction = g_Frame.sunDirection.xyz;
    float3 towardsSun = g_Frame.sunDirection.xyz;
    bool sunFar = leaf && dot(normal, towardsSun) < 0.0f;
    float3 sunSide = sunFar ? -normal : normal;
    if (!(dot(sunSide, towardsSun) > 0.0f) || dot(g_Frame.sunColor.rgb, 1.0f) <= 0.0f) return 0.0f;

    direction = SunSample(random.rand2());
    float3 sunOrigin = sunFar ? OffsetFromTriangle(world, sunSide) : origin;
    float visibility = VisibilityFromSurface(sunOrigin, sunSide, tolerance, direction, 1e4f, ShadowRayTowards(RayTowardsSun));
    if (visibility <= 0.0f) return 0.0f;

    return g_Frame.sunColor.rgb * (visibility * (sunFar ? LeafTransmittance : 1.0f));
}

// With the cosine, its light before the albedo.
float3 SunLight(float3 world, float3 normal, float3 origin, float tolerance, bool leaf, inout Random random)
{
    float3 direction;
    return SunReaching(world, normal, origin, tolerance, leaf, random, direction) * abs(dot(normal, g_Frame.sunDirection.xyz));
}

// The light a hit sends towards `toEye` from the sun, a point or spot light and a self-lit mesh: their diffuse light
// by its albedo, and with path tracing the sun's and the light's specular light by GGX, as the sun pass and the lamps
// shade what the camera sees; a self-lit mesh's specular light is the specular ray that meets it. `origin` and
// `tolerance` as OffsetFromTriangle and CopyTolerance give them for its triangle.
float3 HitOwnLight(HitMaterial material, float3 toEye, float3 origin, float tolerance, inout Random random)
{
    float3 toSun, toLight;
    float3 sun = SunReaching(material.world, material.geometric, origin, tolerance, material.isLeaf, random, toSun);
    float3 lamp = SampledLocalLightReaching(material.world, material.geometric, origin, tolerance, random, toLight);
    float sunCosine = dot(material.normal, toSun);
    float3 light = material.albedo * (sun * (material.isLeaf ? abs(sunCosine) : saturate(sunCosine)) + lamp * saturate(dot(material.normal, toLight)));
    light += material.albedo * SampledEmitterLight(material.world, material.geometric, origin, tolerance, random);
    if (!any(material.specularColor > 0.0f)) return light;

    float3 specular = GgxSpecular(sun, material.normal, toEye, toSun, material.roughness, material.specularColor);
    return light + specular + GgxSpecular(lamp, material.normal, toEye, toLight, material.roughness, material.specularColor);
}

// The side a hit's cosine-weighted ray leaves from, and what its light counts: a leaf's ray goes to its
// far side in proportion to what that side gives, so either side's ray counts the same.
float3 BounceSide(float3 normal, bool leaf, inout Random random, out float weight)
{
    weight = leaf ? 1.0f + LeafTransmittance : 1.0f;
    return leaf && random.rand() * weight >= 1.0f ? -normal : normal;
}

// The light a cache cell's hit reflects from the sun, the sky and the frame's point and spot lights, in the
// game's diffuse light units times albedo; a leaf adds what it lets through from its far side.
float3 HitDirectLight(float3 world, float3 normal, float3 albedo, bool leaf, inout Random random)
{
    float3 origin = OffsetFromTriangle(world, normal);
    float tolerance = CopyTolerance(world);
    float3 light = SunLight(world, normal, origin, tolerance, leaf, random);

    // The sky through a cosine-weighted ray: its radiance is irradiance over pi for that sample.
    float skyWeight;
    float3 skySide = BounceSide(normal, leaf, random, skyWeight);
    float3 skyDirection = mapToCosineHemisphere(random.rand2(), skySide);
    float3 skyOrigin = all(skySide == normal) ? origin : OffsetFromTriangle(world, skySide);
    light += SkyRadiance(skyDirection) * (skyWeight * VisibilityFromSurface(skyOrigin, skySide, tolerance, skyDirection, 1e4f, RayTowardsSurface));

    light += SampledLocalLight(world, normal, origin, tolerance, random);
    light += SampledEmitterLight(world, normal, origin, tolerance, random);
    return albedo * light;
}

#endif
