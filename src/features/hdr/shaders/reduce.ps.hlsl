// In place of CopyReductionX4 on the scene: the scene at a quarter of its size, as bloom and the lens
// flares start from it, through TppTonemap as the game had it. Four bilinear taps, as the game's.
#include "../../../shaders/hdr_common.hlsli"

// CopyReductionX4's offset and scale of its texture coordinates are its c12.
cbuffer cPSObject : register(b5)
{
    float4x4 m_viewWorld;
    float4x4 m_world;
    float4 m_useWeightCount;
    float4 m_localParam[4];
};

Texture2D<float4> inImage : register(t0);
SamplerState g_samplerLinear_Clamp : register(s11);

float4 Tap(float2 uv, int2 offset)
{
    const float4 color = inImage.Sample(g_samplerLinear_Clamp, uv, offset);
    return float4(LinearToSrgb(saturate(Tonemap(SrgbToLinear(color.rgb)))), color.a);
}

float4 main(float4 position : SV_Position, float2 clip : TEXCOORD0) : SV_Target
{
    const float2 uv = (float2(clip.x, -clip.y) * 0.5 + 0.5) * m_localParam[3].zw + m_localParam[3].xy;
    return (Tap(uv, int2(-1, -1)) + Tap(uv, int2(1, -1)) + Tap(uv, int2(-1, 1)) + Tap(uv, int2(1, 1))) / 4;
}
