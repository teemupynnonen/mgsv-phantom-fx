// One thread per froxel: the sun's share in it this frame, from points spread through it, at a new
// place in it each frame, the same along the whole view ray (Hillaire, "Physically Based and Unified
// Volumetric Rendering in Frostbite", 2015). FogMedia adds the last frames' share.
#include "fog_inputs.hlsli"

Texture2D<float> ShadowTexture : register(t1); // The sun's cascades
SamplerComparisonState ShadowSampler : register(s1);
RWTexture3D<float> Visibility : register(u0);

// A view-space point placed in cascade `level` (0 to 3): x and y in -1 to 1 inside it, z towards the sun.
float3 CascadePosition(float3 shadowPosition, int level)
{
    if (level == 0) return shadowPosition;

    int column = level - 1;
    return float3(m_cascades[0][column], m_cascades[1][column], m_cascades[2][column]) + shadowPosition * m_cascades[3][column];
}

// The share of the sun reaching a view-space point: one comparison in the first cascade that holds
// it, as the sun pass picks it, faded to lit towards the end of the shadow range as the sun pass fades.
float SunVisibility(float3 viewPosition)
{
    float4 position = float4(viewPosition, 1);
    float resolution = m_shadowMapResolutions.x;
    float3 shadowPosition = float3(dot(position, m_shadowProjection[0]), dot(position, m_shadowProjection[1]),
        dot(position, m_shadowProjection[2]));
    float area = 1 - 8 / resolution;
    area *= area;

    int level = 3;
    float3 placed = CascadePosition(shadowPosition, 3);
    [unroll] for (int candidate = 2; candidate >= 0; --candidate)
    {
        float3 inCascade = CascadePosition(shadowPosition, candidate);
        if (all(inCascade.xy * inCascade.xy <= area) && inCascade.z >= -1)
        {
            level = candidate;
            placed = inCascade;
        }
    }

    float halfTexel = 0.5 / resolution;
    float row = saturate(level - 1);
    float2 uv = min(float2(placed.x * 0.5 + 0.5, 0.5 - placed.y * 0.5) * 0.5, 1 - halfTexel * 2) + float2(level / 2.0 - row, row * 0.5) + halfTexel;
    float compareDepth = max(m_cascades[0].w * (placed.z + 1), 0.000001);
    float shadow = 1 - ShadowTexture.SampleCmpLevelZero(ShadowSampler, uv, compareDepth);

    float3 fromMaxLevel = viewPosition - float3(0, 0, m_lightParams[8].x);
    float pastShadowStart = max(length(fromMaxLevel) - m_lightParams[8].y, 0);
    float fade = saturate(pastShadowStart * m_lightParams[6].x) * (fromMaxLevel.z >= 0 ? 1.0 : 0.0);
    fade *= fade;

    return saturate((1 - shadow) * fade + shadow - m_lightParams[6].w);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= TileSize)) return;

    float nearDepth = id.z == 0 ? 0 : SliceDepth(id.z - 1);
    float farDepth = SliceDepth(id.z);
    float3 place = ColumnDirection(id.xy + Jitter);
    float visibility = 0;
    for (uint i = 0; i < ShadowSamples; ++i)
        visibility += SunVisibility(place * lerp(nearDepth, farDepth, (i + DepthJitter) / ShadowSamples));
    Visibility[id] = visibility / ShadowSamples;
}
