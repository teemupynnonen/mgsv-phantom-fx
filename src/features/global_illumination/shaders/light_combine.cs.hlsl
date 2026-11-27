// The global illumination's light in place of the game's probe light in its diffuse light. The
// game's light target keeps the probe light, as its exposure measures that target; the upscale
// draws the probe light aside as well, for this to take it out.
#include "../../../shaders/visibility_march.hlsli"

Texture2D<float4> Diffuse : register(t0); // The game's diffuse light, as the resolve reads it
Texture2D<float4> Light : register(t1);   // The traced light, in the same units
Texture2D<float4> Ambient : register(t2); // The game's probe light, which the upscale drew aside
RWTexture2D<float4> OutDiffuse : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;

    float4 diffuse = Diffuse[pixel];
    float3 direct = max(diffuse.rgb - Ambient[pixel].rgb, 0.0f);
    OutDiffuse[pixel] = float4(direct * DiffuseScale + Light[pixel].rgb * LightStrength, diffuse.a);
}
