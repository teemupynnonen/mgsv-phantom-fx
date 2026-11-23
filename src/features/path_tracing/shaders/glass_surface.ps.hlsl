#ifndef COLOR_TEXTURE
#define COLOR_TEXTURE 0
#endif
// Drawn with fox3dfw_glass*'s own vertex shader, after each of its draws. From their disassembly: the normal from the
// normal map, Fresnel by Schlick from the material table's F0, and alpha = the texture's cover x Fresnel x the vertex's
// alpha. Its colour is (ambient + diffuse) x lerp(reflection, lerp(GlassColor, reflection, Fresnel), GlassColor.a) +
// specular, where the reflection is a cube map times ReflectionColor, and with COLOR_TEXTURE times the texture's colour too.
// So the reflection's share of the pixel is (1 - GlassColor.a + GlassColor.a x Fresnel), here with clean glass's reflectance
// in place of the game's Fresnel (below), and the fog's share of that.
#include "../../../shaders/forward_scene_common.hlsli"
#include "surface_output.hlsli"

cbuffer cPSMaterial : register(b4)
{
    float4 m_materials[4]; // [0].x the material table's row, [1] ReflectionColor, [2] GlassColor, [3].y GlassRoughness
};

Texture2D<float4> inDiffuseTexture : register(t0);
Texture2D<float4> inNormalTexture : register(t1);
Texture2D<float4> g_MaterialTexture : register(t10);
SamplerState inDiffuseSampler : register(s0);
SamplerState inNormalSampler : register(s1);
SamplerState g_samplerPoint_Wrap : register(s8);

// The vertex shaders' outputs up to the last read, register for register. `world.w` is the vertex's alpha, `range.y` the view's z.
Surface main(float4 position : SV_Position, float2 uv : TEXCOORD0, float2 range : TEXCOORD1, float4 world : TEXCOORD3, float4 shadow : TEXCOORD4,
    float3 tangent : TEXCOORD5, float3 binormal : TEXCOORD6, float3 vertexNormal : TEXCOORD7)
{
#if COLOR_TEXTURE
    const float4 base = inDiffuseTexture.Sample(inDiffuseSampler, uv);
    const float3 tint = base.rgb;
    const float cover = base.a;
#else
    const float3 tint = 1;
    const float cover = inDiffuseTexture.Sample(inDiffuseSampler, uv).x;
#endif

    const float2 xy = inNormalTexture.Sample(inNormalSampler, uv).ag * 2 - 1;
    const float z = sqrt(saturate(1 - dot(xy, xy)) + 0.0001);
    float3 normal = normalize(normalize(tangent) * xy.x + normalize(binormal) * xy.y + normalize(vertexNormal) * z);

    const float3 toSurface = normalize(world.xyz - m_eyepos.xyz);
    const float3 reflected = reflect(toSurface, normal);
    const float cosine = saturate(dot(normalize(reflected - toSurface), reflected));
    const float F0 = g_MaterialTexture.SampleLevel(g_samplerPoint_Wrap, float2(m_materials[0].x, 0.75), 0).x;
    const float fresnel = saturate(F0 + (1 - F0) * exp2((-5.55473 * cosine - 6.98316) * cosine));

    const float alpha = cover * fresnel * world.w;
    if (alpha <= 1.0 / 512) discard;

    if (dot(normal, toSurface) > 0) normal = -normal;

    // The game's F0 makes a pane as opaque as its cover says, right for what it blends. As the strength of a reflection it is
    // not: the game reflects a cube map times the light on the pane, far dimmer than the surroundings the rays bring back, which
    // turned dark sunglasses into tinted mirrors. The reflection takes what clean glass's two faces reflect instead, the rest
    // of what ClearGlassTransmittance lets through: 8% head-on.
    const float clearGlass = 0.04 + 0.96 * pow(1 - cosine, 5);
    const float sheetReflectance = 1 - (1 - clearGlass) * (1 - clearGlass);

    const float reflectionShare = 1 - m_materials[2].w + m_materials[2].w * fresnel;
    const float reflection = cover * world.w * sheetReflectance * reflectionShare;
    Surface surface;
    surface.depth = position.z;
    surface.normal = float4(normal, m_materials[3].y);
    surface.weight = float4(reflection * tint * m_materials[1].rgb * FogTransmittance(position.xy, range.y), alpha);
    return surface;
}
