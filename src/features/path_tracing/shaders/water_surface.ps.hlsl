#ifndef LAYERED
#define LAYERED 0
#endif
#ifndef TWO_CHANNEL
#define TWO_CHANNEL 0
#endif
#ifndef RIPPLES
#define RIPPLES 0
#endif
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
#include "../../../shaders/forward_scene_common.hlsli"
#include "surface_output.hlsli"

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
