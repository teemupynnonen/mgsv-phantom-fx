// The light's volume as the game drew it, in the same form, from a copy of its indices and vertices.
cbuffer LightPass : register(b0)
{
    float4 ViewToUv;
    uint4 Shadow;
    uint4 Volume; // x: 1 for 32-bit indices, y: the vertices' stride, z: where they start
};

ByteAddressBuffer Drawn : register(t0);

void main(uint id : SV_VertexID, out float4 position : SV_Position, out float4 clip : TEXCOORD0, out float4 sampling : TEXCOORD1)
{
    uint index = Volume.x ? Drawn.Load(id * 4) : (Drawn.Load((id * 2) & ~3u) >> ((id % 2) * 16)) & 0xFFFF;
    position = asfloat(Drawn.Load4(Volume.z + index * Volume.y));
    clip = position;
    sampling = float4((position.xy * float2(0.5, -0.5) + 0.5 * position.w) * ViewToUv.xy + ViewToUv.zw * position.w, position.zw);
}
