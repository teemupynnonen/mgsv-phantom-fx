#ifndef LIGHT
#define LIGHT 0
#endif
#ifndef FINAL
#define FINAL 0
#endif
// Ported from XeGTAO (Intel, MIT; license in licenses/XeGTAO-LICENSE.txt).
// XeGTAO's edge-aware 3 x 3 denoiser, one pixel per thread at the march's size, on the visibility
// (LIGHT 0) or the light (LIGHT 1). The passes before the last weigh the centre less (beta / 5), so
// they blur more, as XeGTAO's do.
#if LIGHT
#define Value float3
#define Stored float3
#else
#define Value float
#define Stored unorm float
#endif

Texture2D<Value> Source : register(t0);
Texture2D<float> Edges : register(t1);
RWTexture2D<Stored> Output : register(u0);

static const float BlurAmount = FINAL ? 1.2 : 1.2 / 5;
static const float DiagonalWeight = 0.85 * 0.5;

float4 unpackEdges(float packed)
{
    uint value = uint(packed * 255.5);
    return saturate(float4((value >> 6) & 3, (value >> 4) & 3, (value >> 2) & 3, value & 3) / 3.0);
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    uint2 size;
    Source.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;

    int2 p = pixel, last = int2(size) - 1;
    int2 left = clamp(p + int2(-1, 0), 0, last), right = clamp(p + int2(1, 0), 0, last);
    int2 top = clamp(p + int2(0, -1), 0, last), bottom = clamp(p + int2(0, 1), 0, last);
    float4 edgesL = unpackEdges(Edges[left]), edgesR = unpackEdges(Edges[right]);
    float4 edgesT = unpackEdges(Edges[top]), edgesB = unpackEdges(Edges[bottom]);

    // Both sides of an edge must agree, then a little leaks through where three or four edges meet,
    // which keeps thin features from aliasing.
    float4 edges = unpackEdges(Edges[p]) * float4(edgesL.y, edgesR.x, edgesT.w, edgesB.z);
    float edginess = saturate(4 - 2.5 - dot(edges, 1)) / (4 - 2.5) * 0.5;
    edges = saturate(edges + edginess);

    float weightTL = DiagonalWeight * (edges.x * edgesL.z + edges.z * edgesT.x);
    float weightTR = DiagonalWeight * (edges.z * edgesT.y + edges.y * edgesR.z);
    float weightBL = DiagonalWeight * (edges.w * edgesB.x + edges.x * edgesL.w);
    float weightBR = DiagonalWeight * (edges.y * edgesR.w + edges.w * edgesB.y);

    Value sum = Source[p] * BlurAmount;
    float weight = BlurAmount;
    sum += Source[left] * edges.x + Source[right] * edges.y + Source[top] * edges.z + Source[bottom] * edges.w;
    weight += dot(edges, 1);
    sum += Source[clamp(p + int2(-1, -1), 0, last)] * weightTL + Source[clamp(p + int2(1, -1), 0, last)] * weightTR;
    sum += Source[clamp(p + int2(-1, 1), 0, last)] * weightBL + Source[clamp(p + int2(1, 1), 0, last)] * weightBR;
    weight += weightTL + weightTR + weightBL + weightBR;

    Output[pixel] = sum / weight;
}
