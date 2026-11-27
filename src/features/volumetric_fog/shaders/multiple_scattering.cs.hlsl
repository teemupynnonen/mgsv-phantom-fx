// Hillaire's multiple scattering lookup table: one thread group per texel, 64 directions each.
#include "atmosphere.hlsli"

RWTexture2D<float4> Output : register(u0);
groupshared float3 MultipleAsOneShared[64];
groupshared float3 LightShared[64];

[numthreads(1, 1, 64)]
void main(uint3 id : SV_DispatchThreadID)
{
    float2 uv = (id.xy + 0.5) / MultipleScatteringSize;
    uv = float2(FromSubUvsToUnit(uv.x, MultipleScatteringSize), FromSubUvsToUnit(uv.y, MultipleScatteringSize));
    float sunZenithCos = uv.x * 2 - 1;
    float3 towardsSun = float3(0, sqrt(saturate(1 - sunZenithCos * sunZenithCos)), sunZenithCos);
    float viewHeight = BottomRadius + saturate(uv.y + PlanetRadiusOffset) * (TopRadius - BottomRadius - PlanetRadiusOffset);
    float3 position = float3(0, 0, viewHeight);

    float i = 0.5 + id.z / 8, j = 0.5 + id.z % 8;
    float theta = 2 * Pi * i / 8, phi = acos(1 - 2 * j / 8);
    float3 direction = float3(cos(theta) * sin(phi), sin(theta) * sin(phi), cos(phi));
    Scattering result = Integrate(position, direction, towardsSun, 20, false, false, false, true);
    MultipleAsOneShared[id.z] = result.multipleAsOne * 4 * Pi / 64;
    LightShared[id.z] = result.light * 4 * Pi / 64;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint half = 32; half > 0; half /= 2)
    {
        if (id.z < half)
        {
            MultipleAsOneShared[id.z] += MultipleAsOneShared[id.z + half];
            LightShared[id.z] += LightShared[id.z + half];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (id.z > 0) return;

    // Every order of scattering after the first, as a geometric series.
    float3 multipleAsOne = MultipleAsOneShared[0] / (4 * Pi);
    float3 secondOrder = LightShared[0] / (4 * Pi);
    Output[id.xy] = float4(secondOrder / (1 - multipleAsOne), 1);
}
