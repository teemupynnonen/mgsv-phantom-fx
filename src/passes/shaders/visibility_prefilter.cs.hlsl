// XeGTAO's depth prefilter: the distances the march samples at the render size, and four smaller
// copies, each a 2 x 2 reduction of the one before (XeGTAO's depth MIPs). The march reads farther
// samples from smaller copies: across up to 15% of the render height, full-size reads missed the
// cache on nearly every sample. Each 8 x 8 group covers 16 x 16 pixels. The sign carries the
// foliage material, negative on foliage, so the march reads one texel a sample.
#include "../../shaders/visibility_march.hlsli"

cbuffer Scene : register(b1)
{
    float4 Scene[30];
};

Texture2D<float> Depth : register(t0);     // Reversed-Z, 0 on the sky
// The G-buffer's specular target, whose z is the material; unbound until a sun pass has bound it.
Texture2D<float4> Specular : register(t1);
RWTexture2D<float> OutDistance0 : register(u0);
RWTexture2D<float> OutDistance1 : register(u1);
RWTexture2D<float> OutDistance2 : register(u2);
RWTexture2D<float> OutDistance3 : register(u3);
RWTexture2D<float> OutDistance4 : register(u4);

// Fox Engine's material for grass, bushes and tree leaves, in 255ths.
static const float FoliageMaterial = 43;
// Far enough to occlude nothing, and finite, so the reduction can weigh it.
static const float SkyDistance = 1e6;

groupshared float scratch[8][8];

float signedDistanceAt(uint2 pixel)
{
    pixel = min(pixel, Size - 1);
    float depth = Depth[pixel];
    float distance = depth > 0 ? Scene[21].z / (depth - Scene[21].w) : SkyDistance;
    return round(Specular[pixel].z * 255) == FoliageMaterial ? -distance : distance;
}

// XeGTAO's DepthMIPFilter: the weighted mean of four distances, fading out those well in front of
// the farthest, so a near edge does not pull a coarse texel forward off the surface behind it. The
// texel is foliage where foliage has at least half the weight.
float reduce(float4 values)
{
    float4 distances = abs(values);
    float farthest = max(max(distances.x, distances.y), max(distances.z, distances.w));
    float falloffRange = 0.615 * 0.75 * Radius, falloffFrom = 0.75 * Radius - falloffRange;
    float4 weights = saturate(1 - (farthest - distances - falloffFrom) / falloffRange);
    float weightSum = dot(weights, 1);
    float distance = dot(weights, distances) / weightSum;
    return dot(weights, float4(values < 0)) * 2 >= weightSum ? -distance : distance;
}

// Reduces the 2 x 2 values `stride` apart in scratch, from the top left at `local`.
float reduceScratch(uint2 local, uint stride)
{
    return reduce(float4(scratch[local.x][local.y], scratch[local.x + stride][local.y], scratch[local.x][local.y + stride],
        scratch[local.x + stride][local.y + stride]));
}

// Writes past the edges are dropped, and reads clamp to them.
[numthreads(8, 8, 1)]
void main(uint2 thread : SV_DispatchThreadID, uint2 local : SV_GroupThreadID)
{
    uint2 pixel = thread * 2;
    float4 values = float4(signedDistanceAt(pixel), signedDistanceAt(pixel + uint2(1, 0)), signedDistanceAt(pixel + uint2(0, 1)),
        signedDistanceAt(pixel + uint2(1, 1)));
    OutDistance0[pixel] = values.x;
    OutDistance0[pixel + uint2(1, 0)] = values.y;
    OutDistance0[pixel + uint2(0, 1)] = values.z;
    OutDistance0[pixel + uint2(1, 1)] = values.w;

    float reduced = reduce(values);
    OutDistance1[thread] = reduced;
    scratch[local.x][local.y] = reduced;

    // Each level, one thread in four reduces the 2 x 2 block it tops. Only that thread reads its own
    // cell, so it may overwrite it without waiting.
    GroupMemoryBarrierWithGroupSync();
    if (all(local % 2 == 0))
    {
        reduced = reduceScratch(local, 1);
        OutDistance2[thread / 2] = reduced;
        scratch[local.x][local.y] = reduced;
    }

    GroupMemoryBarrierWithGroupSync();
    if (all(local % 4 == 0))
    {
        reduced = reduceScratch(local, 2);
        OutDistance3[thread / 4] = reduced;
        scratch[local.x][local.y] = reduced;
    }

    GroupMemoryBarrierWithGroupSync();
    if (all(local % 8 == 0)) OutDistance4[thread / 8] = reduceScratch(local, 4);
}
