// The far blur's light at half the render size, in linear light. What is in focus stays out, so it cannot
// spread into the blur around it, and each pixel counts less the brighter it is (Karis's weight), so one
// noisy ray cannot turn into a disc. a = the radius in half-size texels; rgb is multiplied by it, which
// lets a bilinear tap beside an in-focus texel keep its colour: rgb / a.
#include "dof_common.hlsli"

Texture2D<float4> Scene : register(t1);
Texture2D<float2> Coverage : register(t2);

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 corner = int2(position.xy) * 2;

    float3 light = 0;
    float radius = 0, total = 0;
    for (int i = 0; i < 4; ++i)
    {
        const int3 texel = int3(corner + int2(i & 1, i >> 1), 0);
        const float pixelRadius = Coverage.Load(texel).y;
        if (pixelRadius <= SharpRadius) continue;

        const float3 color = SrgbToLinear(Scene.Load(texel).rgb);
        const float weight = 1 / (1 + max(color.r, max(color.g, color.b)));
        light += color * weight;
        radius += pixelRadius * weight;
        total += weight;
    }

    if (total == 0) return 0;

    const float halfSize = radius / total / 2;

    return float4(light / total * halfSize, halfSize);
}
