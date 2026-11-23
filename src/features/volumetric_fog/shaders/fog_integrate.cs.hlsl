// One thread per texel column: adds up the slices front to back and writes each into its tile of
// the game's fog texture, as the game's fog pass writes it. In the physical look the last slice
// holds the height fog alone, which the sky and the clouds read: the sky's light already holds the
// air in front of it.
#include "fog_inputs.hlsli"

Texture3D<float4> Media : register(t0);
Texture3D<float4> FogOnly : register(t1);
RWTexture2D<float4> GameFog : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= TileSize)) return;

    float normalizeFactor = FogParam[1].y;
    if (!Physical)
    {
        float3 selfLuminance = FogMedium[7].xyz;
        float3 color = 0;
        for (uint slice = 0; slice < Slices; ++slice)
        {
            float4 media = Media[uint3(id.xy, slice)];
            color += media.rgb;
            uint2 tile = uint2(slice % 32, slice / 32);
            GameFog[tile * TileSize + id.xy] = saturate(float4((color + (1 - media.a) * selfLuminance) / normalizeFactor, media.a));
        }
        return;
    }

    float3 color = 0, fogColor = 0;
    float transmittance = 1, fogTransmittance = 1;
    for (uint slice = 0; slice < Slices; ++slice)
    {
        float4 media = Media[uint3(id.xy, slice)];
        float4 fogOnly = FogOnly[uint3(id.xy, slice)];
        color += transmittance * media.rgb;
        transmittance *= media.a;
        fogColor += fogTransmittance * fogOnly.rgb;
        fogTransmittance *= fogOnly.a;
        uint2 tile = uint2(slice % 32, slice / 32);
        float4 written = slice < Slices - 1 ? float4(color, transmittance) : float4(fogColor, fogTransmittance);
        GameFog[tile * TileSize + id.xy] = float4(max(written.rgb, 0) / normalizeFactor, saturate(written.a));
    }
}
