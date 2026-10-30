// The ray-traced scene's kernels (docs/DEVELOPMENT.md, "Ray-traced scene"): what the scene learnt about
// its new geometries started over, the game's terrain and characters into the geometry pool, and a debug
// view of the scene from the camera.
#include "gi_common.hlsli"

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
// Ring g_Dispatch.x, a cell a thread: its two triangles into the pool from the ring's first vertex,
// clockwise seen from above: their front faces up, the only side the rays see (GI_RAY_FLAGS; the
// other order hid the ground from above in the scene view and showed it from below).
// Cells the ring inside covers are left inactive (NaN positions), so the rings do not overlap.
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

    uint first = g_Frame.terrainRingOffsets[ring] + 16 * 6 * (did.y * cells + did.x);
    g_GeometryPoolOut.Store4(first, asuint(float4(p00, 1.0f)));
    g_GeometryPoolOut.Store4(first + 16, asuint(float4(p01, 1.0f)));
    g_GeometryPoolOut.Store4(first + 32, asuint(float4(p10, 1.0f)));
    g_GeometryPoolOut.Store4(first + 48, asuint(float4(p10, 1.0f)));
    g_GeometryPoolOut.Store4(first + 64, asuint(float4(p01, 1.0f)));
    g_GeometryPoolOut.Store4(first + 80, asuint(float4(p11, 1.0f)));
}
