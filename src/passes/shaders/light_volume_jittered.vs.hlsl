// The game's Lighting_*_vs and SSLighting2_*_vs, with the volume moved by the jitter first: the clip-space
// position twice, then where it samples the light targets.
cbuffer cVSSystem : register(b0)
{
    float4 m_param;
    float4 m_renderInfo;
    float4 m_renderBuffer;
    float4 m_dominantLightDir;
};

cbuffer cVSScene : register(b2)
{
    float4 m_registers[28];
    float4 m_cameraCenterOffset; // xy: the jitter in NDC (camera::applyJitter)
};

void main(float4 inPosition : POSITION, out float4 position : SV_Position, out float4 clip : TEXCOORD0, out float4 sampling : TEXCOORD1)
{
    position = inPosition;
    position.xy += m_cameraCenterOffset.xy * position.w;
    clip = position;
    float4 viewToUv = m_renderInfo * m_renderBuffer.zwzw;
    float2 screen = position.xy * float2(0.5, -0.5) + 0.5 * position.w;
    sampling = float4(screen * viewToUv.xy + viewToUv.zw * position.w, position.zw);
}
