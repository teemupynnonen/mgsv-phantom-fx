#pragma once

// HLSL for the river's light under the global illumination (river_light.hpp), compiled at run time with d3dcompiler_47.
namespace phantom_fx::river_light::shaders
{
// Drawn with the river's own vertex shader, right after the two draws aside. Their outputs are the game's: the colour encoded
// as sRGB, times the alpha it blends by, and that alpha. The ambient's share is their difference, linear; the part of it the
// traced light takes away comes off the scene with a reversed subtraction.
inline constexpr char Remove[] = R"(
cbuffer Parameters : register(b6)
{
    float2 ToLightUv;   // 1 / the river's target size
    float2 LightTexel;  // 1 / the traced light's size
    float Strength;     // The global illumination's
    float3 Padding;
};

Texture2D<float4> Lit : register(t7);         // The river with its ambient
Texture2D<float4> Unlit : register(t8);       // The river without it
Texture2D<float4> TracedLight : register(t9); // At the render size, in the game's diffuse light's units
Texture2D<float4> ProbeLight : register(t10); // The game's, in the same
SamplerState g_samplerLinear_Clamp : register(s11);

float3 Decode(float3 encoded)
{
    return encoded <= 0.04045 ? encoded / 12.92 : pow(max((encoded + 0.055) / 1.055, 0), 2.4);
}

float3 Encode(float3 light)
{
    return light <= 0.003131 ? light * 12.92 : 1.055 * pow(max(light, 0.00001), 1 / 2.4) - 0.055;
}

float Luminance(float3 colour)
{
    return dot(colour, float3(0.2126, 0.7152, 0.0722));
}

// The river's inputs up to SV_Position, so it lands where the vertex shader puts it.
float4 main(float4 color : COLOR0, float4 position : SV_Position) : SV_Target
{
    const int3 pixel = int3(position.xy, 0);
    const float4 lit = Lit.Load(pixel);
    if (!(lit.a > 1.0 / 512)) discard;

    const float3 withAmbient = Decode(lit.rgb / lit.a);
    const float3 withoutAmbient = Decode(Unlit.Load(pixel).rgb / lit.a);

    // The bed's traced light over the game's: the light the water would have under the canopy. Wide, 5 x 5 texels 8 apart: the
    // traced light is Ray Reconstruction's noisy input, which it cleans only in the final image, and over water its guides are
    // a mirror's, so a ratio over 3 x 3 texels 4 apart left the river grainy (1.7x its mean in high-frequency noise, against
    // 0.6x without it, 2026-11-02). The water's ambient changes slowly across it.
    const float2 uv = position.xy * ToLightUv;
    float traced = 0, probe = 0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x)
        {
            const float2 at = uv + float2(x, y) * 8 * LightTexel;
            traced += Luminance(TracedLight.SampleLevel(g_samplerLinear_Clamp, at, 0).rgb);
            probe += Luminance(ProbeLight.SampleLevel(g_samplerLinear_Clamp, at, 0).rgb);
        }
    const float kept = probe > 1e-5 ? saturate(traced * Strength / probe) : 1;

    const float3 corrected = withoutAmbient + kept * max(withAmbient - withoutAmbient, 0);
    return float4(max(lit.rgb - Encode(corrected) * lit.a, 0), 0);
}
)";
}
