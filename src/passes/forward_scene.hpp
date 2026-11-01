#pragma once

// HLSL the shaders drawn over the game's forward draws share (glass, mirrors, rivers, their caustics), put before their own:
// what those draws have bound.
namespace phantom_fx::forward_scene
{
inline constexpr char Common[] = R"(
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

// The view's z of the scene behind a river, from the game's depth at t13 (ReconstructViewZ).
float SceneViewZ(Texture2D<float> depth, float2 position)
{
    return m_projectionParam.z / (depth.Load(int3(position, 0)) - m_projectionParam.w);
}
)";
}
