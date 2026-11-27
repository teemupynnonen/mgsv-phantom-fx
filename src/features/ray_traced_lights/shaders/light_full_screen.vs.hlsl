// The full-screen triangle in the light passes' vertex shaders' form (Lighting_*_vs): the clip-space
// position twice, then where it samples the light targets, from the viewport within them.
cbuffer LightPass : register(b0)
{
    float4 ViewToUv; // xy: the viewport's size over the targets'; zw: its corner over the targets'
    uint4 Shadow;    // y: 1 when the light was traced, so the share holds its shadow
};

void main(uint id : SV_VertexID, out float4 position : SV_Position, out float4 clip : TEXCOORD0, out float4 sampling : TEXCOORD1)
{
    float2 xy = float2(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0);
    position = float4(xy, 0.0, 1.0);
    clip = position;
    sampling = float4((xy * float2(0.5, -0.5) + 0.5) * ViewToUv.xy + ViewToUv.zw, 0.0, 1.0);
}
