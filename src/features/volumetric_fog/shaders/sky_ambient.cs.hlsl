// The mean light from all around, which lights the height fog: the sky above, and the ground below,
// lit by the sun through the air and by the sky; under the game's cloud deck, its light.
#include "atmosphere.hlsli"

RWTexture2D<float4> Output : register(u0); // SkyParameters
groupshared float4 Sums[64];

[numthreads(8, 8, 1)]
void main(uint3 id : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    float4 sun = Output[uint2(SunTexel, 0)];
    float3 towardsSun = Output[uint2(DirectionTexel, 0)].xyz;
    float zenithCos = (id.y + 0.5) / 8, zenithSin = sqrt(1 - zenithCos * zenithCos);
    float azimuth = 2 * Pi * (id.x + 0.5) / 8;
    float3 direction = float3(zenithSin * cos(azimuth), zenithCos, zenithSin * sin(azimuth));
    Sums[index] = float4(SkyLight(direction, towardsSun, sun.a), 1);
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint half = 32; half > 0; half /= 2)
    {
        if (index < half) Sums[index] += Sums[index + half];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index > 0) return;

    float3 sky = sun.rgb * Sums[0].rgb / Sums[0].a;
    float3 sunlight = sun.rgb * TransmittanceToTop(BottomRadius + sun.a, towardsSun.y) * saturate(towardsSun.y);
    float3 ground = GroundAlbedo * (sunlight / Pi + sky);
    // The deck's light is the game's fog light, which already holds the ground's.
    float3 deck = Output[uint2(OvercastTexel, 0)].rgb;
    Output[uint2(AmbientTexel, 0)] = float4(0.5 * (sky + ground) + deck, 0);
}
