// The transformed fields into the maps the surface reads, each tile a slice: the displacement, the slopes
// with the sideways stretch, and the slopes' squares with the foam, whose levels of detail then give the
// slopes' spread over a pixel (LEAN mapping: Olano and Baker, 2010; for the sea, Bruneton et al., "Real-time
// Realistic Ocean Lighting using Seamless Transitions from Geometry to BRDF", 2010). Foam gathers where the
// surface folds (the Jacobian of the displacement falls low) and fades over seconds.
#include "sea_common.hlsli"

RWStructuredBuffer<float4> g_Spectrum : register(u0);
RWTexture2DArray<float4> g_Displacement : register(u1); // Sideways x, height, sideways z
RWTexture2DArray<float4> g_Derivatives : register(u2);  // dh/dx, dh/dz, dDx/dx, dDz/dz
RWTexture2DArray<float4> g_Moments : register(u3);      // (dh/dx)^2, (dh/dz)^2, squeezed, foam
RWStructuredBuffer<float> g_Foam : register(u4);        // The foam kept from frame to frame, N x N a tile
RWStructuredBuffer<uint> g_Deepest : register(u5);      // Each tile's deepest trough below the level, metres (float bits)

groupshared uint g_GroupDeepest;

static const float FoamBelow = 0.55; // The Jacobian below which crests break
static const float FoamSharpness = 4;
static const float SqueezedBelow = 0.9; // The Jacobian below which the game's foam patterns start to show

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    // The transform sums e^(i 2 pi m n / N) from the spectrum's first texel; its wave vectors start at
    // -N / 2, which flips the sign of every other texel.
    float sign = ((id.x + id.y) & 1) ? -1 : 1;
    float4 first = g_Spectrum[SpectrumIndex(id.xy, id.z * 2)] * sign;
    float4 second = g_Spectrum[SpectrumIndex(id.xy, id.z * 2 + 1)] * sign;

    float dx = first.x * g_Choppiness, height = first.y, dz = first.z * g_Choppiness, hx = first.w;
    float hz = second.x, dxx = second.y * g_Choppiness, dzz = second.z * g_Choppiness, dxz = second.w * g_Choppiness;
    float jacobian = (1 + dxx) * (1 + dzz) - dxz * dxz;
    // The smallest tile's ripples do not break.
    uint kept = SpectrumIndex(id.xy, id.z);
    float foam = g_Foam[kept] * g_FoamLeft;
    // Where the surface is squeezed, short of folding: the crests' fronts, which the game's whitecap mask covers.
    // The setting reaches both the folds and the squeeze, so 0 leaves no foam at all, not just the squeeze's. Its
    // whole range counts: above 1 the squeeze reaches further out, while the folds are already all foam at 1.
    float squeezed = 0;
    float coverage = max(g_FoamCoverage, 0);
    if (id.z < CASCADES - 1 && coverage > 0)
    {
        foam = max(foam, saturate((FoamBelow - jacobian) * FoamSharpness) * min(coverage, 1));
        // Higher waves fold more and show their foam over more of the sea; the setting widens that reach.
        float squeezedBelow = FoamBelow + (SqueezedBelow - FoamBelow) * coverage;
        squeezed = saturate((squeezedBelow - jacobian) / (squeezedBelow - FoamBelow));
    }
    else
    {
        foam = 0;
    }
    g_Foam[kept] = foam;

    g_Displacement[id] = float4(dx, height, dz, 0);
    g_Derivatives[id] = float4(hx, hz, dxx, dzz);
    g_Moments[id] = float4(hx * hx, hz * hz, squeezed, foam);

    // The ray-traced scene's plane lies under the waves (sea::planeLevel): the tiles' troughs together are as deep as
    // the surface goes. Positive floats order as their bits do; a group's deepest first, then one atomic a group.
    if (index == 0) g_GroupDeepest = 0;
    GroupMemoryBarrierWithGroupSync();
    InterlockedMax(g_GroupDeepest, asuint(max(-height * g_HeightScale, 0)));
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) InterlockedMax(g_Deepest[id.z], g_GroupDeepest);
}
