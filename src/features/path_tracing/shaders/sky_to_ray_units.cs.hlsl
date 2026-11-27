// The face's texture holds what the scene would: exposed, and sRGB encoded beyond 1 (docs/DEVELOPMENT.md, "HDR").
Texture2DArray<float4> Drawn : register(t0);
RWTexture2DArray<float4> Output : register(u0);

cbuffer Parameters : register(b0)
{
    uint Face;
    uint Size;
    float Scale; // From the exposed light to the rays' units
    float Most;
};

float3 Decoded(float3 encoded)
{
    float3 high = pow((max(encoded, 0) + 0.055) / 1.055, 2.4);
    return encoded <= 0.04045 ? encoded / 12.92 : high;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= Size)) return;

    Output[uint3(id, Face)] = float4(clamp(Decoded(Drawn.Load(int4(id, Face, 0)).rgb), 0, Most) * Scale, 1);
}
