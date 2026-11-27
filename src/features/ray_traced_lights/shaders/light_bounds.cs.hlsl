// A light's volume's bounds in the world, from the copy of its indices and vertices (clip space, as the
// game drew it) through the inverse of the light pass's m_projectionView.
cbuffer Bounds : register(b0)
{
    float4 ClipToWorld[4]; // Rows applied as dot(row, v)
    uint4 Volume;          // x: 1 for 32-bit indices, y: the vertices' stride, z: where they start, w: how many indices
    uint4 Slot;            // x: the light's place in Bounds
};

ByteAddressBuffer Drawn : register(t0);
RWStructuredBuffer<float4> Bounds : register(u0); // Two a light: lowest, highest

[numthreads(1, 1, 1)]
void main()
{
    float3 lowest = 1e30, highest = -1e30;
    for (uint i = 0; i < Volume.w; ++i)
    {
        uint index = Volume.x ? Drawn.Load(i * 4) : (Drawn.Load((i * 2) & ~3u) >> ((i % 2) * 16)) & 0xFFFF;
        float4 clip = asfloat(Drawn.Load4(Volume.z + index * Volume.y));
        float4 world = float4(dot(ClipToWorld[0], clip), dot(ClipToWorld[1], clip), dot(ClipToWorld[2], clip), dot(ClipToWorld[3], clip));
        float3 corner = world.xyz / world.w;
        lowest = min(lowest, corner);
        highest = max(highest, corner);
    }

    Bounds[Slot.x * 2] = float4(lowest, 1.0);
    Bounds[Slot.x * 2 + 1] = float4(highest, 1.0);
}
