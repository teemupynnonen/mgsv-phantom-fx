// Opacity micromaps (experiment): a triangle's 4^level micro-triangles lie along a space-filling curve over
// its barycentric grid (the DXR spec's "Opacity micromap encoding"). The curve's walk is the one NVIDIA's
// Opacity Micro-Map SDK and OptiX give (bird::index2dbary); the hardware test in the experiment checked
// it against the GPU's own lookup.
#ifndef GI_MICROMAP_HLSLI
#define GI_MICROMAP_HLSLI

uint MicromapEvenBits(uint x)
{
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0F0F0F0Fu;
    x = (x | (x >> 4)) & 0x00FF00FFu;
    x = (x | (x >> 8)) & 0x0000FFFFu;
    return x;
}

uint MicromapPrefixXor(uint x)
{
    x ^= x >> 1;
    x ^= x >> 2;
    x ^= x >> 4;
    x ^= x >> 8;
    return x;
}

// The corners of micro-triangle `index` of 4^level as barycentrics: DXR's, the second and third
// vertices' weights.
void MicroTriangle(uint index, uint level, out float2 a, out float2 b, out float2 c)
{
    if (level == 0)
    {
        a = float2(0.0f, 0.0f);
        b = float2(1.0f, 0.0f);
        c = float2(0.0f, 1.0f);
        return;
    }

    uint b0 = MicromapEvenBits(index), b1 = MicromapEvenBits(index >> 1);
    uint fx = MicromapPrefixXor(b0), fy = MicromapPrefixXor(b0 & ~b1);
    uint t = fy ^ b1;
    uint mask = (1u << level) - 1u;
    uint u = ((fx & ~t) | (b0 & ~t) | (~b0 & ~fx & t)) & mask;
    uint v = (fy ^ b0) & mask;
    uint w = ((~fx & ~t) | (b0 & ~t) | (~b0 & fx & t)) & mask;
    bool isUpright = ((u ^ v ^ w) & 1u) != 0;

    float step = 1.0f / float(1u << level);
    float edge = step;
    if (!isUpright)
    {
        u += 1u;
        v += 1u;
        edge = -step;
    }

    a = float2(u, v) * step;
    b = a + float2(edge, 0.0f);
    c = a + float2(0.0f, edge);
}

#endif
