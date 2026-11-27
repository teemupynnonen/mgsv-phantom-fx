#ifndef UPGRADE_TONEMAP
#define UPGRADE_TONEMAP 0
#endif
// In place of the game's grading pass (Tonemap_ps). The scene holds light, which goes through
// TppTonemap as the game had it, then the game's own grading: the bloom screened over it and the
// 16x16x16 colour LUT; in SDR that is the image. With HDR output (UPGRADE_TONEMAP) the light the
// tonemap took is then given back as RenoDX's UpgradeToneMap does (MIT, RenoDX-LICENSE.txt): added to
// the graded luminance, with the graded colour scaled to it, so highlights keep the grade's colour.
// Below the curve's knee nothing was taken, so the image is the game's. Alpha keeps the game's luma,
// which FXAA reads.
#include "../../../shaders/hdr_common.hlsli"

cbuffer cPSSystem : register(b0)
{
    float4 m_param;
    float4 m_renderInfo;
    float4 m_renderBuffer;
    float4 m_dominantLightDir;
};

Texture2D<float4> inImage : register(t0);
Texture2D<float4> inColorLUT : register(t6);
Texture2D<float4> inBloom : register(t7);
SamplerState g_samplerPoint_Wrap : register(s8);
SamplerState g_samplerLinear_Clamp : register(s11);

float3 ColorCorrection(float3 color)
{
    const float3 uvw = saturate(color) * 0.9375;
    const float slice = floor(uvw.b * 16) * 0.0625;
    const float blend = (uvw.b - slice) * 16;
    const float2 uv = float2(uvw.r * 0.0625 + slice, uvw.g) + float2(1.0 / 512, 1.0 / 32);
    const float3 low = inColorLUT.Sample(g_samplerLinear_Clamp, uv).rgb;
    const float3 high = inColorLUT.Sample(g_samplerLinear_Clamp, uv + float2(0.0625, 0)).rgb;
    return low * (1 - blend) + high * blend;
}

float4 main(float2 clip : TEXCOORD0, float4 position : SV_Position) : SV_Target
{
    const float2 pixelSize = m_renderBuffer.zw;
    const float2 bloomUv = float2(clip.x, -clip.y) * 0.5 + 0.5 + pixelSize * 2;
    const float3 light = min(SrgbToLinear(inImage.Sample(g_samplerPoint_Wrap, position.xy * pixelSize).rgb), HalfMax);
    const float3 toned = saturate(Tonemap(light));
    const float3 scene = LinearToSrgb(toned);

    // The game screens the bloom over the scene with gamma 2 standing in for sRGB.
    const float3 squared = scene * scene;
    const float3 bloom = min(inBloom.Sample(g_samplerLinear_Clamp, bloomUv).rgb, 0.5);
    float3 color = saturate(squared + bloom - squared * bloom);
    color *= rsqrt(max(color, 1.0 / 512));
    color = ColorCorrection(color);
    const float luma = dot(color, float3(0.299, 0.587, 0.114));

#if !UPGRADE_TONEMAP
    return float4(color, luma);
#else
    const float3 graded = SrgbToLinear(color);
    const float taken = max(Luminance(light) - Luminance(toned), 0);
    const float gradedLuminance = Luminance(graded);
    const float scale = gradedLuminance > 0 ? (gradedLuminance + taken) / gradedLuminance : 0;
    return float4(LinearToSrgb(graded * scale), luma);
#endif
}
