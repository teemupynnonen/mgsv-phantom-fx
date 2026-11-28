// The ground's cloud shadow (clouds.cpp draws it once a frame, before the sky): for each texel of a square around the eye,
// the sun's transmittance along its ray through the layer. The texel is where that ray crosses the layer's base; the
// density is clouds' own (cloud_density.hlsli), so a shadow lies under the cloud that casts it.
#include "cloud_density.hlsli"
static const int ShadowSteps = 32;

float4 main(float4 color : COLOR0, float4 uv : TEXCOORD0, float4 world : TEXCOORD1, float4 position : SV_Position) : SV_Target
{
    float3 eye = m_scene[20].xyz;
    float2 centre = CloudShadowCentre(eye);
    float2 xz = centre + (position.xy / ShadowSize - 0.5) * ShadowExtent;
    float3 start = float3(xz.x, LayerBase, xz.y);

    float3 towardsSun = normalize(m_materials[0].xyz);
    // A sun at the horizon would send the ray through the layer for hundreds of kilometres: past this it is treated as set.
    float rise = max(towardsSun.y, 0.1);
    float pathLength = (LayerTop - LayerBase) / rise;
    float dt = pathLength / ShadowSteps;

    float2 windXZ = CloudWindXZ();
    float3 wind = CloudWind();
    float baseCover = CloudBaseCover(1);
    float opticalDepth = 0;
    for (int i = 0; i < ShadowSteps; ++i)
    {
        float3 where = start + towardsSun * (dt * (i + 0.5));
        opticalDepth += saturate(CloudDensity(where, windXZ, wind, baseCover, 30, true) * 4) * dt;
    }
    // A cloud's shadow is never black: the sun still reaches the ground as the sky's light, scattered through the cloud.
    // The menu's strength: 0 leaves the ground lit, 1 the shipped look, and below 1 the shadow only softens the sun.
    float transmittance = max(exp(-opticalDepth * Extinction * 1.5 * max(g_cloudShadow, 0.0)), 0.3);
    return float4(transmittance, 0, 0, 1);
}