// The ray-traced scene's kernels (docs/DEVELOPMENT.md, "Ray-traced scene"): what the scene learnt about
// its new geometries started over, the game's terrain and characters into the geometry pool, and a debug
// view of the scene from the camera.
#include "gi_common.hlsli"
#include "gi_micromap.hlsli"

// Each new geometry starts what was learnt about it (its albedo) over.
[numthreads(64, 1, 1)]
void ClearNewGeometries(uint did : SV_DispatchThreadID)
{
    if (did < g_Frame.counts.x) g_GeometryAlbedo[g_NewGeometries[did]] = 0;
}

// The frame's characters (gi_renderer.cpp): their meshes to pose, and their palettes, three rows a bone
// (each the game's 3x4 skinning matrix: the model's space, the bind pose taken out).
StructuredBuffer<GISkinRecord> g_SkinRecords : register(t12);
StructuredBuffer<float4> g_Palettes : register(t13);

// The record holding a vertex of the frame's characters: records are sorted by their first vertex and adjoin.
uint FindSkinRecord(uint vertex)
{
    uint low = 0, high = g_Dispatch.y;
    while (high - low > 1)
    {
        uint middle = (low + high) / 2;
        if (g_SkinRecords[middle].firstVertex <= vertex) low = middle;
        else high = middle;
    }

    return low;
}

float3 PaletteTransform(uint bone, float3 position)
{
    float4 p = float4(position, 1.0f);
    return float3(dot(g_Palettes[3 * bone], p), dot(g_Palettes[3 * bone + 1], p), dot(g_Palettes[3 * bone + 2], p));
}

float3 PaletteRotation(uint bone, float3 direction)
{
    return float3(dot(g_Palettes[3 * bone].xyz, direction), dot(g_Palettes[3 * bone + 1].xyz, direction), dot(g_Palettes[3 * bone + 2].xyz, direction));
}

// One thread a vertex of the frame's characters (g_Dispatch.x of them, in g_Dispatch.y records): its
// bind pose under its bones' palette matrices, weighted, into its posed place, as the game's skinning
// shaders place it, its normal and tangent turned with it. The pool is still in the unordered access state,
// so it is read through its output.
[numthreads(64, 1, 1)]
void Skin(uint did : SV_DispatchThreadID)
{
    if (did >= g_Dispatch.x) return;

    GISkinRecord record = g_SkinRecords[FindSkinRecord(did)];
    uint vertex = did - record.firstVertex;
    float4 bind = asfloat(g_GeometryPoolOut.Load4(record.bindOffset + 16 * vertex));
    uint4 skin = g_GeometryPoolOut.Load4(record.skinOffset + 16 * vertex);
    uint bones[4] = {skin.x & 0xFFFFu, skin.x >> 16, skin.y & 0xFFFFu, skin.y >> 16};
    uint4 frame = record.bindNormalOffset ? g_GeometryPoolOut.Load4(record.bindNormalOffset + 16 * vertex) : uint4(0, 0, 0, 0);
    float3 bindNormal = float3(f16tof32(frame.x), f16tof32(frame.x >> 16), f16tof32(frame.y));
    float3 bindTangent = float3(f16tof32(frame.z), f16tof32(frame.z >> 16), f16tof32(frame.w));
    float3 position = 0.0f, normal = 0.0f, tangent = 0.0f;
    float total = 0.0f;
    for (uint i = 0; i < 4; ++i)
    {
        float weight = ((skin.z >> (8 * i)) & 0xFFu) / 255.0f;
        if (weight == 0.0f) continue;

        position += weight * PaletteTransform(record.firstBone + bones[i], bind.xyz);
        normal += weight * PaletteRotation(record.firstBone + bones[i], bindNormal);
        tangent += weight * PaletteRotation(record.firstBone + bones[i], bindTangent);
        total += weight;
    }

    // The 8-bit weights' rounding leaves their sum near one: they are scaled to it.
    position = total > 0.0f ? position / total : bind.xyz;
    g_GeometryPoolOut.Store4(record.posedOffset + 16 * vertex, asuint(float4(position, bind.w)));
    if (record.posedNormalOffset == 0u) return;

    uint3 n = f32tof16(total > 0.0f ? normal : bindNormal), t = f32tof16(total > 0.0f ? tangent : bindTangent);
    g_GeometryPoolOut.Store4(record.posedNormalOffset + 16 * vertex, uint4(n.x | (n.y << 16), n.z, t.x | (t.y << 16), t.z | (frame.w & 0xFFFF0000u)));
}

// The traced scene from the camera, shaded by its normals; red where it disagrees with the game's
// depth, magenta and black stripes where the game drew something the scene lacks, yellow where the
// scene has something in the game's sky. Stripes, as plain magenta is the shade of a surface facing
// down: a ward's ceiling read as missing (2026-10-26).
[numthreads(8, 8, 1)]
void DebugScene(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    float3 farPoint = DepthPosition(did, 1e-6f);
    float3 direction = normalize(farPoint - g_Frame.eye.xyz);
    SceneHit hit = TraceClosest(g_Frame.eye.xyz, direction, 1e5f);

    float depth = g_Depth.Load(int3(did, 0));
    float gameDistance = depth > 0.0f ? distance(DepthPosition(did, depth), g_Frame.eye.xyz) : FLT_MAX;

    float3 color = float3(0.02f, 0.03f, 0.08f);
    if (hit.hit)
    {
        float3 normal = HitNormal(hit, direction);
        color = (0.5f * normal + 0.5f) * 0.7f;
        if (depth <= 0.0f) color = lerp(color, float3(1.0f, 0.9f, 0.0f), 0.6f);
        else if (abs(hit.distance - gameDistance) > 0.05f * gameDistance + 0.1f) color = lerp(color, float3(1.0f, 0.0f, 0.0f), 0.6f);
    }
    else if (depth > 0.0f)
        color = ((did.x + did.y) / 8u) % 2u ? float3(1.0f, 0.0f, 1.0f) : 0.0f;

    g_Output[did] = float4(color, 1.0f);
}

// ---- The game's terrain (TerrainPoint in gi_common.hlsli) ----
// The game's own terrain patches the rings take this frame (scene_contents.cpp), ring after ring (terrainPatchRings).
StructuredBuffer<GITerrainPatch> g_TerrainPatches : register(t9);

// Whether one of a ring's patches covers its cell from `lowest` to `highest` (world x, z) whole: the patch's own triangles
// stand there, as the game drew them, and the ring's are left out.
bool IsUnderPatch(uint ring, float2 lowest, float2 highest)
{
    uint4 patches = g_Frame.terrainPatchRings[ring];
    for (uint i = patches.y; i < patches.y + patches.z; ++i)
    {
        float4 rect = g_TerrainPatches[i].worldRect;
        if (all(lowest >= rect.xy - GI_TERRAIN_PATCH_SLACK) && all(highest <= rect.zw + GI_TERRAIN_PATCH_SLACK)) return true;
    }

    return false;
}

// Ring g_Dispatch.x, a cell a thread: its two triangles into the pool from the ring's first vertex,
// clockwise seen from above: their front faces up, the only side the rays see (GI_RAY_FLAGS; the
// other order hid the ground from above in the scene view and showed it from below).
// Cells the ring inside covers are left inactive (NaN positions), so the rings do not overlap. A ring's cells under the
// game's own patches collapse to a point, no triangle there: the patches stand there instead, the same structure's
// second geometry (TerrainPatches). Still active, so the ring's refits keep working.
[numthreads(8, 8, 1)]
void TerrainGrid(uint2 did : SV_DispatchThreadID)
{
    uint ring = g_Dispatch.x;
    float4 r = g_Frame.terrainRings[ring];
    uint cells = uint(r.w);
    if (any(did >= cells)) return;

    float2 corner = r.xy + float2(did) * r.z;
    bool hole = false;
    if (ring > 0)
    {
        float4 inner = g_Frame.terrainRings[ring - 1];
        float2 lowest = inner.xy, highest = inner.xy + inner.z * inner.w;
        hole = all(corner >= lowest - 1e-3f) && all(corner + r.z <= highest + 1e-3f);
    }

    float3 p00 = TerrainPoint(corner), p10 = TerrainPoint(corner + float2(r.z, 0.0f));
    float3 p01 = TerrainPoint(corner + float2(0.0f, r.z)), p11 = TerrainPoint(corner + r.z);
    if (hole) p00 = p10 = p01 = p11 = asfloat(0x7FC00000u);
    else if (IsUnderPatch(ring, corner, corner + r.z)) p10 = p01 = p11 = p00;

    uint first = g_Frame.terrainRingOffsets[ring] + 16 * 6 * (did.y * cells + did.x);
    g_GeometryPoolOut.Store4(first, asuint(float4(p00, 1.0f)));
    g_GeometryPoolOut.Store4(first + 16, asuint(float4(p01, 1.0f)));
    g_GeometryPoolOut.Store4(first + 32, asuint(float4(p10, 1.0f)));
    g_GeometryPoolOut.Store4(first + 48, asuint(float4(p10, 1.0f)));
    g_GeometryPoolOut.Store4(first + 64, asuint(float4(p01, 1.0f)));
    g_GeometryPoolOut.Store4(first + 80, asuint(float4(p11, 1.0f)));
}

// The patch of a ring's (`patches`, terrainPatchRings) holding a triangle: they are sorted by their first triangle and adjoin.
uint FindTerrainPatch(uint4 patches, uint triangleIndex)
{
    uint low = patches.y, high = patches.y + patches.z;
    while (high - low > 1)
    {
        uint middle = (low + high) / 2;
        if (g_TerrainPatches[middle].firstTriangle <= triangleIndex) low = middle;
        else high = middle;
    }

    return low;
}

// Ring g_Dispatch.x, one thread a triangle of its patches' room: the game's own terrain patches as its main camera drew them, a draw's
// index range of the game's patch mesh at the draw's rect of the virtual heightmap, each vertex placed as the game's
// terrain vertex shader places it (TerrainVertex), so the scene's ground is the game's: within 0.06 mm on a slope, where
// the 1 m ring cells stood up to 3 cm off it; elsewhere some triangles still lie 3 mm under the game's, for a cause not
// found (2026-11-06: not the textures, samplers, constants, transforms, indices or pixel depth). Past the frame's
// triangles, inactive (NaN). The front faces up, as the rings', whichever way the game's indices turn.
[numthreads(64, 1, 1)]
void TerrainPatches(uint did : SV_DispatchThreadID)
{
    if (did >= GI_TERRAIN_PATCH_TRIANGLES) return;

    uint4 patches = g_Frame.terrainPatchRings[g_Dispatch.x];
    uint first = patches.x + 48 * did; // Three float4 vertices a triangle
    GITerrainPatch last = g_TerrainPatches[patches.y + max(patches.z, 1u) - 1];
    if (patches.z == 0 || did >= last.firstTriangle + last.indexCount / 3)
    {
        float4 inactive = float4(asfloat(0x7FC00000u).xxx, 1.0f);
        g_GeometryPoolOut.Store4(first, asuint(inactive));
        g_GeometryPoolOut.Store4(first + 16, asuint(inactive));
        g_GeometryPoolOut.Store4(first + 32, asuint(inactive));
        return;
    }

    GITerrainPatch patch = g_TerrainPatches[FindTerrainPatch(patches, did)];
    uint firstIndex = patch.firstIndex + 3 * (did - patch.firstTriangle);
    float3 p[3];
    for (uint i = 0; i < 3; ++i)
    {
        uint vertex = g_GeometryPoolOut.Load(g_Frame.terrainPatchMesh.x + 4 * (firstIndex + i)) + patch.baseVertex;
        float2 own = asfloat(g_GeometryPoolOut.Load2(g_Frame.terrainPatchMesh.y + 8 * vertex));
        p[i] = TerrainVertex(own * patch.uvScaleOffset.xy + patch.uvScaleOffset.zw, patch.worldScale.xyz, patch.worldOffset.xyz);
    }

    if (cross(p[1] - p[0], p[2] - p[0]).y < 0.0f)
    {
        float3 swap = p[1];
        p[1] = p[2];
        p[2] = swap;
    }

    g_GeometryPoolOut.Store4(first, asuint(float4(p[0], 1.0f)));
    g_GeometryPoolOut.Store4(first + 16, asuint(float4(p[1], 1.0f)));
    g_GeometryPoolOut.Store4(first + 32, asuint(float4(p[2], 1.0f)));
}

// ---- Opacity micromaps (gi_scene.hpp) ----
StructuredBuffer<GIMicromapJob> g_MicromapJobs : register(t8);
RWByteAddressBuffer g_MicromapBake : register(u3);

// The job holding a group of the frame's dispatch: jobs are sorted by their first group and adjoin.
uint FindMicromapJob(uint group)
{
    uint low = 0, high = g_Dispatch.y;
    while (high - low > 1)
    {
        uint middle = (low + high) / 2;
        if (g_MicromapJobs[middle].firstGroup <= group) low = middle;
        else high = middle;
    }

    return low;
}

// A micro-triangle's state (D3D12_RAYTRACING_OPACITY_MICROMAP_STATE) from its texture's alpha at points over
// it, a barycentric grid of four steps with its corners and edges: opaque or transparent where they all
// agree, unknown otherwise, which the rays' alpha test then settles (CutOutBlocks). Detail finer than the
// grid can slip between its points.
uint MicroTriangleState(Texture2D<float4> texture, float threshold, float2 uv0, float2 uv1, float2 uv2, float2 a, float2 b, float2 c)
{
    uint opaque = 0, samples = 0;
    for (uint i = 0; i <= 4; ++i)
        for (uint j = 0; j <= 4 - i; ++j)
        {
            float2 barycentrics = a + (b - a) * (i / 4.0f) + (c - a) * (j / 4.0f);
            float2 uv = (1.0f - barycentrics.x - barycentrics.y) * uv0 + barycentrics.x * uv1 + barycentrics.y * uv2;
            if (texture.SampleLevel(g_WrapSampler, uv, 0.0f).a >= threshold) ++opaque;
            ++samples;
        }

    if (opaque == samples) return 1u;
    if (opaque == 0) return 0u;

    return opaque * 2 >= samples ? 3u : 2u;
}

groupshared uint gs_SeenStates;

// One group a triangle of the frame's jobs (g_Dispatch.x groups, 65535 a row, over g_Dispatch.y jobs):
// its micro-triangles' states, sixteen a word, then its micromap's description and its index, the
// uniform one where every micro-triangle agrees.
[numthreads(64, 1, 1)]
void BakeMicromaps(uint3 gid : SV_GroupID, uint thread : SV_GroupIndex)
{
    uint group = gid.y * 65535u + gid.x;
    if (group >= g_Dispatch.x) return;

    GIMicromapJob job = g_MicromapJobs[FindMicromapJob(group)];
    uint primitive = group - job.firstGroup;
    GIGeometry geometry = job.geometry;
    uint3 i = TriangleIndices(geometry, primitive);
    float2 uv0 = VertexUV(geometry, i.x), uv1 = VertexUV(geometry, i.y), uv2 = VertexUV(geometry, i.z);
    Texture2D<float4> texture = g_SceneTextures[NonUniformResourceIndex(geometry.texture - 1)];

    if (thread == 0) gs_SeenStates = 0;
    GroupMemoryBarrierWithGroupSync();

    uint microTriangles = 1u << (2u * job.level);
    uint bytes = microTriangles / 4u;
    uint seen = 0;
    for (uint word = thread; word < microTriangles / 16u; word += 64u)
    {
        uint packed = 0;
        for (uint k = 0; k < 16u; ++k)
        {
            float2 a, b, c;
            MicroTriangle(word * 16u + k, job.level, a, b, c);
            uint state = MicroTriangleState(texture, geometry.alphaThreshold, uv0, uv1, uv2, a, b, c);
            packed |= state << (2u * k);
            seen |= 1u << state;
        }

        g_MicromapBake.Store(job.dataOffset + primitive * bytes + 4u * word, packed);
    }

    InterlockedOr(gs_SeenStates, seen);
    GroupMemoryBarrierWithGroupSync();
    if (thread != 0) return;

    // Its states are found from the bake buffer's start, the micromap arrays' input.
    g_MicromapBake.Store2(job.descOffset + 8u * primitive, uint2(job.dataOffset + primitive * bytes, job.level | (GI_MICROMAP_FORMAT_4_STATE << 16)));
    // D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX: -1 fully transparent, -2 fully opaque.
    int index = int(primitive);
    if (gs_SeenStates == 2u) index = -2;
    else if (gs_SeenStates == 1u) index = -1;
    g_MicromapBake.Store(job.indexOffset + 4u * primitive, asuint(index));
}
