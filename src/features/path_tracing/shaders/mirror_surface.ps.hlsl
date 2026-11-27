// Drawn with fox3dfw_parallax_refrection's own vertex shader, after each of its draws: alpha = its mask's y x the vertex's
// alpha, with the fog over a painted picture of the room, unlit. A mirror is flat and its vertex shader gives no normal, so
// the normal comes from the view-space position across the pixel.
#include "../../../shaders/forward_scene_common.hlsli"
#include "surface_output.hlsli"

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
