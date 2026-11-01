#pragma once

// HLSL for glass and mirrors on Direct3D 11 (glass.hpp), compiled at runtime with d3dcompiler_47.
namespace phantom_fx::glass::shaders
{
// Where glass dominates, tells Ray Reconstruction of it, as its guide asks of a window: depth, normal and hit distance
// from the one surface. The normal and the hit distance go into its guides; `SurfaceDepth` is left with the depth of those
// pixels alone, for the upscaler's motion (upscaler_shaders.hpp, Inputs). Below this cover what is behind the pane dominates.
inline constexpr char ReconstructionGuides[] = R"(
Texture2D<float4> SurfaceNormal : register(t0);
Texture2D<float4> SurfaceWeight : register(t1);
Texture2D<float4> SurfaceReflection : register(t2);
RWTexture2D<float> SurfaceDepth : register(u0);
RWTexture2D<float4> Normals : register(u1);
RWTexture2D<float> HitDistance : register(u2);

static const float DominantCover = 0.1;

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (SurfaceWeight[pixel].a < DominantCover)
    {
        SurfaceDepth[pixel] = 0;
        return;
    }

    Normals[pixel] = SurfaceNormal[pixel];
    HitDistance[pixel] = SurfaceReflection[pixel].a;
}
)";

// What the surfaces' shaders share: the game's constants, and what they write into the surface targets.
inline constexpr char SurfaceCommon[] = R"(
cbuffer cPSScene : register(b2)
{
    float4 m_sceneRows[8];
    float4 m_view[3];          // World to view, rows
    float4 m_sceneRows2[9];
    float4 m_eyepos;
    float4 m_projectionParam;  // 1 / P00, 1 / P11, B, A
    float4 m_viewportSize;
    float4 m_exposure;
    float4 m_fogParam[2];      // [1].x: 1 / log2 of the fog's far distance
};

Texture2D<float4> g_tex_fog : register(t12);
SamplerState g_samplerLinear_Clamp : register(s11);

struct Surface
{
    float depth : SV_Target0;
    float4 normal : SV_Target1; // World space, the roughness in w
    float4 weight : SV_Target2; // rgb: how much of what it reflects reaches the eye; a: how much it covers what is behind
};

// How much of a surface's light gets through the fog, as GetVolumetricFog reads it: alpha, between two of the 128 tiles
// a slice.
float FogTransmittance(float2 position, float viewZ)
{
    const float layer = saturate(log2(viewZ) * m_fogParam[1].x) * 127;
    const float2 clip = (position - 0.5) / m_viewportSize.xy * float2(2, -2) + float2(-1, 1);
    const float2 uv = clip * float2(0.014648, 0.123047) + float2(0.015625, 0.125);
    const float2 slices = float2(floor(layer), floor(min(layer + 1, 127))) / 32;
    const float2 column = frac(slices) * 32 * 0.03125, row = floor(slices) * 0.25;
    const float near = g_tex_fog.Sample(g_samplerLinear_Clamp, uv + float2(column.x, row.x)).a;
    const float far = g_tex_fog.Sample(g_samplerLinear_Clamp, uv + float2(column.y, row.y)).a;
    return lerp(near, far, frac(layer));
}
)";

// Drawn with fox3dfw_glass*'s own vertex shader, after each of its draws. From their disassembly: the normal from the
// normal map, Fresnel by Schlick from the material table's F0, and alpha = the texture's cover x Fresnel x the vertex's
// alpha. Its colour is (ambient + diffuse) x lerp(reflection, lerp(GlassColor, reflection, Fresnel), GlassColor.a) +
// specular, where the reflection is a cube map times ReflectionColor, and with COLOR_TEXTURE times the texture's colour too.
// So the reflection gets alpha x (1 - GlassColor.a + GlassColor.a x Fresnel) of the pixel, and the fog's share of that.
inline constexpr char GlassSurface[] = R"(
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

    const float reflectionShare = 1 - m_materials[2].w + m_materials[2].w * fresnel;
    Surface surface;
    surface.depth = position.z;
    surface.normal = float4(normal, m_materials[3].y);
    surface.weight = float4(alpha * reflectionShare * tint * m_materials[1].rgb * FogTransmittance(position.xy, range.y), alpha);
    return surface;
}
)";

// Drawn with fox3dfw_parallax_refrection's own vertex shader, after each of its draws: alpha = its mask's y x the vertex's
// alpha, with the fog over a painted picture of the room, unlit. A mirror is flat and its vertex shader gives no normal, so
// the normal comes from the view-space position across the pixel.
inline constexpr char MirrorSurface[] = R"(
Texture2D<float4> inMaskTexture : register(t2);
SamplerState inMaskSampler : register(s2);

// `extra`: y the view's z, z the vertex's alpha.
Surface main(float4 position : SV_Position, float4 uv : TEXCOORD0, float3 unused : TEXCOORD1, float3 viewInTangent : TEXCOORD2, float3 extra : TEXCOORD3)
{
    const float alpha = inMaskTexture.Sample(inMaskSampler, uv.zw).y * extra.z;
    if (alpha <= 1.0 / 512) discard;

    const float2 ndc = float2(position.x / m_viewportSize.x * 2 - 1, 1 - position.y / m_viewportSize.y * 2);
    const float3 view = float3(ndc * m_projectionParam.xy * extra.y, extra.y);
    float3 normal = normalize(cross(ddx(view), ddy(view)));
    if (dot(normal, view) > 0) normal = -normal;

    Surface surface;
    surface.depth = position.z;
    surface.normal = float4(normalize(normal.x * m_view[0].xyz + normal.y * m_view[1].xyz + normal.z * m_view[2].xyz), 0);
    surface.weight = float4((alpha * FogTransmittance(position.xy, extra.y)).xxx, alpha);
    return surface;
}
)";

// A river or another liquid, drawn with the game's own vertex shader after its draw: its depth, no normal (the
// trace takes the slope from the depth's neighbours, so no shader interface can go wrong), and 0.3 of what it
// reflects, which the trace scales by the water's reflectance at the eye's angle.
inline constexpr char WaterSurface[] = R"(
cbuffer Parameters : register(b0)
{
    float Roughness; // The menu's (features/water_reflections)
    float3 Padding;
};

Surface main(float4 position : SV_Position)
{
    Surface surface;
    surface.depth = position.z;
    // xyz 0: water, the trace takes the slope from the depth's neighbours. Its roughness spreads the traced
    // reflection as the ripples would: the game's normal map is screen-space and driven by a flow map, and the trace
    // cannot read it, so the spread stands in for the detail.
    surface.normal = float4(0.0f, 0.0f, 0.0f, Roughness);
    surface.weight = float4(0.3f, 0.3f, 0.3f, 1.0f);
    return surface;
}
)";

// What glass and mirrors reflect added over the scene, once the game drew them all, as they would have blended it: the
// scene holds sRGB-encoded values here, past 1 too, and the game's glass adds its encoded colour by its alpha.
inline constexpr char Composite[] = R"(
cbuffer Parameters : register(b0)
{
    float Strength; // The menu's
    float3 Padding;
};

Texture2D<float4> Scene : register(t0);
Texture2D<float4> Weight : register(t1);     // The surfaces' weights
Texture2D<float4> Reflection : register(t2); // What they reflect, in the scene's units
Texture2D<float> Shadow : register(t3);     // The sun's traced shadow on the surface itself (ray_tracing::forwardShadow)
Texture2D<float4> SurfaceNormal : register(t4); // The surfaces' normal: zero xyz marks water

float4 main(float4 position : SV_Position) : SV_Target
{
    const int3 pixel = int3(position.xy, 0);
    const float4 weight = Weight.Load(pixel);
    if (!(weight.a > 0)) discard;

    const float3 reflected = Reflection.Load(pixel).rgb * Strength * weight.rgb / weight.a;
    const float3 encoded = reflected <= 0.003131 ? reflected * 12.92 : 1.055 * pow(max(reflected, 0.00001), 1 / 2.4) - 0.055;
    // The surface dims in the sun's shade, as the water does: a pane is mostly what it reflects and the sky behind
    // it, so blocking the sun barely changes it. Water takes no shadow here: the caustics and its light are the bed's,
    // drawn under it before it blends (forward_shadows' caustics), and what it reflects is added untouched.
    const bool water = all(SurfaceNormal.Load(pixel).xyz == 0.0f);
    const float shade = water ? 1.0f : lerp(0.85f, 1.0f, Shadow.Load(pixel));
    return float4(Scene.Load(pixel).rgb * lerp(1.0f, shade, weight.a) + weight.a * encoded, 1);
}
)";
}
