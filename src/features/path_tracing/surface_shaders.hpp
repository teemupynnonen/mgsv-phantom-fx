#pragma once

// HLSL for the forward surfaces on Direct3D 11 (forward_surfaces.hpp), compiled at run time with d3dcompiler_47. The surfaces' own
// shaders go after forward_scene::Common and SurfaceOutput.
namespace phantom_fx::surface_shaders
{
// Where a surface dominates, tells Ray Reconstruction of it, as its guide asks of a window: depth, normal and hit distance
// from the one surface. The normal and the hit distance go into its guides; `SurfaceDepth` is left with the depth of those
// pixels alone, for the upscaler's motion (upscaler_shaders.hpp, Inputs). Below this cover what is behind a pane dominates;
// water always does, as it hides or colours what is under it.
inline constexpr char ReconstructionGuides[] = R"(
Texture2D<float4> SurfaceNormal : register(t0);
Texture2D<float4> SurfaceWeight : register(t1);
Texture2D<float4> SurfaceReflection : register(t2);
RWTexture2D<float> SurfaceDepth : register(u0);
RWTexture2D<float4> Normals : register(u1);
RWTexture2D<float> HitDistance : register(u2);

static const float DominantCover = 0.1;
static const float WaterRoughness = 0.05; // As the trace's (reflections.hlsl)

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const float4 normal = SurfaceNormal[pixel];
    const bool isWater = normal.w < 0;
    if (!isWater && SurfaceWeight[pixel].a < DominantCover)
    {
        SurfaceDepth[pixel] = 0;
        return;
    }

    Normals[pixel] = float4(normal.xyz, isWater ? WaterRoughness : normal.w);
    HitDistance[pixel] = SurfaceReflection[pixel].a;
}
)";

// What the surfaces' shaders write into the surface targets.
inline constexpr char SurfaceOutput[] = R"(
struct Surface
{
    float depth : SV_Target0;
    float4 normal : SV_Target1; // World space, the roughness in w
    float4 weight : SV_Target2; // rgb: how much of what it reflects reaches the eye; a: how much it covers what is behind
};
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

// A river or another liquid, drawn with the game's own vertex shader after its draw: its depth, its ripples' normal as the game's
// shader builds it, and how much of what it reflects shows. That share goes in the normal's w, negative, which marks water: the
// weights blend as glass does, and a pane drawn over a river must stand alone there, not on top of the water's share.
//
// The share follows what the game's shader gives its own reflection, but for the cube map's weight: the water's cover (the base
// texture's alpha, the vertex's, the fade at its shore, NComputeDepthBlendFactor) and the fog in front of it.
//
// The normal, from the disassembly of all twelve Prim_TppPrimRiver*/Prim_TppRiverModel* pixel shaders: the normal map
// (t1, s10) twice, at the mesh's UV times a material scale plus the material's scrolling offset (cPSMaterial[0]), the
// second at 0.97 of its scale; each 2 x texel - 1, summed and normalized, then out of the vertex shader's tangent frame.
// LAYERED (Tl, Ol): the scales at cPSMaterial[6] and the frame at TEXCOORD6-8; else [3] and TEXCOORD5-7. TWO_CHANNEL
// (Hnm): the texel's w and y, z rebuilt. RIPPLES (Rpl): the rain's rings over it, as NAddRippleNormal adds them. Every input
// is declared in the vertex shader's order, so each lands in its register.
inline constexpr char WaterSurface[] = R"(
// What the water shows of its reflectance: the composite adds it in the scene's encoded values, where the whole of it reads as a
// mirror.
static const float ReflectionShare = 0.3f;

cbuffer cPSMaterial : register(b4)
{
    float4 m_materials[7]; // [0]: the two layers' UV offsets; [2]: y the ripples' strength, zw a texel; [3].xy or [6].xy: their scales
};

cbuffer cPSObject : register(b5)
{
    float4 m_objectRows[11];
    float4 m_shoreFade;    // x: 1 / the depth of water it takes to cover the bed
};

Texture2D<float4> inBaseTexture : register(t0);
Texture2D<float4> inNormalTexture : register(t1);
#if RIPPLES
cbuffer cPSSystem : register(b0)
{
    float4 m_param;
    float4 m_renderInfo; // xy: the size the screen's UV divides by
};
// Where each of the four *Rpl shaders reads its rings: the layered ones (Prim_TppPrimRiverTl*Rpl) at t6, the others
// (Prim_TppRiverModelSh*Rpl) at t4, where t6 is not theirs and holds whatever an earlier draw left.
#if LAYERED
Texture2D<float4> inRippleTexture : register(t6);
#else
Texture2D<float4> inRippleTexture : register(t4);
#endif
#endif
Texture2D<float> g_depth : register(t13);
SamplerState g_samplerLinear_Wrap : register(s10);

#if LAYERED
#define FRAME_TANGENT TEXCOORD6
#define FRAME_BINORMAL TEXCOORD7
#define FRAME_NORMAL TEXCOORD8
static const int ScaleRow = 6;
#else
#define FRAME_TANGENT TEXCOORD5
#define FRAME_BINORMAL TEXCOORD6
#define FRAME_NORMAL TEXCOORD7
static const int ScaleRow = 3;
#endif

float3 LayerNormal(float2 uv)
{
    const float4 texel = inNormalTexture.Sample(g_samplerLinear_Wrap, uv);
#if TWO_CHANNEL
    const float2 xy = texel.wy * 2 - 1;
    const float z = sqrt(saturate(1 - dot(xy, xy)) + 0.0001f);
    return float3(xy, z);
#else
    return texel.xyz * 2 - 1;
#endif
}

#if RIPPLES
float3 WithRipples(float3 normal, float2 position)
{
    const float2 uv = position / m_renderInfo.xy;
    const float here = inRippleTexture.Sample(g_samplerLinear_Wrap, uv).x;
    const float right = inRippleTexture.Sample(g_samplerLinear_Wrap, uv + float2(m_materials[2].z, 0)).x;
    const float below = inRippleTexture.Sample(g_samplerLinear_Wrap, uv + float2(0, m_materials[2].w)).x;
    float3 ripple = float3(here - right, 0, here - below);
    ripple.y = 2 - max(abs(ripple.x), abs(ripple.z));
    ripple = normalize(ripple);

    const float3 up = float3(0, 1, 0);
    // The view's first column, as NAddRippleNormal reads it (GetRowT), not its first row.
    const float3 forward = normalize(cross(up, float3(m_view[0].x, m_view[1].x, m_view[2].x)));
    const float3 side = cross(forward, up);
    normal.xz += m_materials[2].y * float2(dot(side, ripple), dot(forward, ripple));
    return normalize(normal);
}
#endif

Surface main(float4 color : COLOR0, float4 position : SV_Position, float2 uv : TEXCOORD0, float3 basePosition : TEXCOORD1,
    float4 shadowPosition : TEXCOORD2, float4 viewPosition : TEXCOORD3, float3 worldPosition : TEXCOORD4,
    float3 tangent : FRAME_TANGENT, float3 binormal : FRAME_BINORMAL, float3 vertexNormal : FRAME_NORMAL)
{
    const float uncovered = 1 - saturate((SceneViewZ(g_depth, position.xy) - basePosition.z) * m_shoreFade.x);
    const float cover = (1 - uncovered * uncovered * uncovered) * color.a * inBaseTexture.Sample(g_samplerLinear_Wrap, uv).a;
    if (cover <= 1.0f / 512) discard;

    const float4 offsets = m_materials[0];
    const float2 scales = m_materials[ScaleRow].xy;
    const float3 local = normalize(LayerNormal(offsets.xy + scales.x * uv) + LayerNormal(offsets.zw + scales.y * 0.97f * uv));
    float3 normal = local.x * tangent + local.y * binormal + local.z * vertexNormal;
    // A degenerate frame: the water lies flat.
    normal = all(isfinite(normal)) && dot(normal, normal) > 1e-8f ? normalize(normal) : float3(0, 1, 0);
#if RIPPLES
    normal = WithRipples(normal, position.xy);
#endif

    Surface surface;
    surface.depth = position.z;
    surface.normal = float4(normal, -ReflectionShare * cover * FogTransmittance(position.xy, basePosition.z));
    surface.weight = 0;
    return surface;
}
)";

// What the surfaces reflect added over the scene, once the game drew them all, as they would have blended it: the scene holds
// sRGB-encoded values here, past 1 too, and the game's glass adds its encoded colour by its alpha.
inline constexpr char Composite[] = R"(
cbuffer Parameters : register(b0)
{
    float Strength; // The menu's
    float3 Padding;
};

Texture2D<float4> Scene : register(t0);
Texture2D<float4> Weight : register(t1);        // The surfaces' weights
Texture2D<float4> Reflection : register(t2);    // What they reflect, in the scene's units
Texture2D<float4> SurfaceNormal : register(t3); // Water's w is minus its share

float4 main(float4 position : SV_Position) : SV_Target
{
    const int3 pixel = int3(position.xy, 0);
    const float waterShare = -SurfaceNormal.Load(pixel).w;
    const float4 weight = waterShare > 0 ? float4(waterShare.xxx, 1) : Weight.Load(pixel);
    if (!(weight.a > 0)) discard;

    const float3 reflected = Reflection.Load(pixel).rgb * Strength * weight.rgb / weight.a;
    const float3 encoded = reflected <= 0.003131 ? reflected * 12.92 : 1.055 * pow(max(reflected, 0.00001), 1 / 2.4) - 0.055;
    return float4(Scene.Load(pixel).rgb + weight.a * encoded, 1);
}
)";
}
