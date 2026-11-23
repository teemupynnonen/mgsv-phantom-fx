// The physical look's sky, in place of the game's sky draws (Sky_Draw_TppBaked, Sky_Draw_Tpp,
// Sky_Draw_TppNoSky), with their vertex shaders and bindings: the sky's light from the sky-view
// table and the game's cloud deck, the game's sun or moon sprite (MOON 1), the height fog in front of
// it from the fog's last slice, and the game's TppTonemap and encoding, or the encoding alone with the
// scene in float (FLOAT_SCENE 1), which takes TppTonemap out of it; clamped to 1 with HDR output
// (HDR_OUTPUT 1). Without the game's dither, as its own sky draws.
#define LINEAR_CLAMP_SAMPLER s11
#include "atmosphere.hlsli"

#define LINEAR_CLAMP_SAMPLER s11

cbuffer cPSSystem : register(b0)
{
    float4 m_param, m_renderInfo, m_renderBuffer, m_dominantLightDir;
};

cbuffer cPSScene : register(b2)
{
    float4 m_scene[26]; // [25].y the fog's normalising factor
};

cbuffer cPSMaterial : register(b4)
{
    float4 m_materials[8]; // [1].xyz the TppTonemap curve
};

SamplerState MoonSampler : register(s0);
Texture2D<float4> MoonTexture : register(t0);
Texture2D<float4> GameFog : register(t12);

float3 TppTonemap(float3 color, float3 curve)
{
    float3 above = curve.y + curve.z - 1 / ((color + curve.z - curve.y) * curve.x);
    return color <= curve.y ? color : above;
}

float3 Encode(float3 linearColor)
{
    float3 high = 1.055 * pow(max(linearColor, 0.00001), 1 / 2.4) - 0.055;
    return linearColor <= 0.0031308 ? linearColor * 12.92 : high;
}

float4 main(float4 position : SV_Position, float3 ray : TEXCOORD0, float4 moonPlace : TEXCOORD1, float4 moonColor : TEXCOORD2) : SV_Target
{
    float4 sun = SkyParameters.Load(int3(SunTexel, 0, 0));
    float3 towardsSun = SkyParameters.Load(int3(DirectionTexel, 0, 0)).xyz;
    float4 deck = SkyParameters.Load(int3(OvercastTexel, 0, 0));
    // The sun's share in sun.rgb is what the deck leaves of the sky.
    float3 color = SkyLight(normalize(ray), towardsSun, sun.a) * sun.rgb + deck.rgb;

#if MOON
    float4 moon = moonColor * MoonTexture.Sample(MoonSampler, moonPlace.zw);
    moon.a *= 1 - deck.a;
    if (all(moonPlace.zw >= 0 && moonPlace.zw <= 1)) color = color * (1 - moon.a) + saturate(moon.a * moon.rgb);
#endif

    float2 pixel = position.xy - 0.5;
    float2 clip = pixel / m_renderInfo.xy * float2(2, -2) + float2(-1, 1);
    float4 fog = GameFog.SampleLevel(LinearClamp, clip * float2(0.014648, 0.123047) + float2(0.015625, 0.125) + float2(0.96875, 0.75), 0);
    color = fog.rgb * m_scene[25].y + color * fog.a;

#if HDR_OUTPUT
    // Clamped to 1 as the game's sky draws stay with HDR output (hdr.cpp): above it the sun's sprite
    // shows as a ring around a dimmer centre instead of a disc. One unit is the game's white.
    return float4(Encode(min(max(color, 0), 1)), 0);
#elif FLOAT_SCENE
    // In SDR the grading's tonemap and clamp to 1 make the image this made (hdr.cpp, withoutTonemap).
    // 65504 is the most the half-float scene holds.
    return float4(Encode(min(max(color, 0), 65504)), 0);
#else
    return float4(Encode(saturate(TppTonemap(color, m_materials[1].xyz))), 0);
#endif
}
