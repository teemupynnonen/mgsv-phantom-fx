// A triangle over the whole target, with the texture coordinate across it.
void main(uint vertex : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((vertex << 1) & 2, vertex & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
