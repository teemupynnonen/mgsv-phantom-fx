// The full-resolution resolve: a depth-joint bilateral upsample of the filtered gather, scaled by
// Amount and added to the game's diffuse light. A plain bilinear would pull the background's
// bounce onto the foreground across a silhouette, which reads as a halo exactly where the eye is.
#include "ssgi_common.hlsli"
Texture2D<float4> Filtered : register(t0); // At the gather's size
Texture2D<float> Depth : register(t1);     // Reversed-Z, 0 on the sky
Texture2D<float4> Diffuse : register(t2);  // The game's diffuse light, as the resolve reads it
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel = tid.xy;
    if (any(pixel >= Size)) return;

    const float scale = max(Scale, 1.0);
    const int2 gs = (int2(Size) + (int)scale - 1) / (int)scale;
    const float2 position = (float2(pixel) + 0.5) / scale - 0.5;
    const int2 base = (int2)floor(position);
    const float2 fraction = frac(position);

    const float z = distanceOf(Depth.Load(int3(pixel, 0)));
    const bool geometry = hasGeometry(z);
    const float invZ = geometry ? 1.0 / z : 0.0;

    float3 radiance = 0;
    float weightSum = 0;
    [unroll]
    for (int y = 0; y <= 1; ++y)
    {
        [unroll]
        for (int x = 0; x <= 1; ++x)
        {
            const int2 q = clamp(base + int2(x, y), int2(0, 0), gs - 1);
            const float4 tap = Filtered.Load(int3(q, 0));
            const float bilinear = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);

            const int2 full = clamp((int2)floor((float2(q) + 0.5) * scale), int2(0, 0), int2(Size) - 1);
            const float qz = distanceOf(Depth.Load(int3(full, 0)));
            const float depthMatch = (geometry && qz > 0.0) ? 1.0 / (1.0 + 32.0 * abs(qz - z) * invZ) : 1.0;

            const float weight = bilinear * depthMatch;
            radiance += tap.rgb * weight;
            weightSum += weight;
        }
    }

    const float3 bounce = radiance / max(weightSum, 1.0e-4) * Amount;
    const float4 diffuse = Diffuse.Load(int3(pixel, 0));
    const float3 output = Debug > 0.5 ? bounce : diffuse.rgb * DiffuseScale + bounce;
    Out[pixel] = float4(output, diffuse.a);
}
