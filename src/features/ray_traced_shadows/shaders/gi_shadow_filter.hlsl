// The spatial passes of AMD's FidelityFX Shadow Denoiser for the sun's ray-traced shadows
// (gi_shadow.hlsli): three edge-avoiding à-trous passes, 1, 2 and 4 pixels apart.
//
// Where it deviates from the denoiser's sample, and why:
// - Half precision is full precision: the kernels compile without 16-bit types.
// - The depth similarity's sigma grows with the distance, 1 m plus 5% of the depth, where the
//   sample's is 1 m: MGSV's views reach kilometres, where neighbouring pixels of sloping ground lie
//   metres apart in depth.
#define float16_t float
#define float16_t2 float2
#define float16_t3 float3
#include "gi_shadow.hlsli"

// Per pixel, set before the pass.
static float s_DepthSigma;

float FFX_DNSR_Shadows_GetDepthSimilaritySigma()
{
    return s_DepthSigma;
}

// The pass's input: the temporal pass's result, the first pass's, then the second's.
float2 FFX_DNSR_Shadows_ReadInput(int2 pixel)
{
    int2 clamped = ClampToShadow(pixel);
    return g_Dispatch.x == 1 ? g_Shadow_History[clamped] : g_Shadow_Reprojection[clamped];
}

// The denoiser as AMD wrote it; its bitfield helpers shift an untyped literal.
#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"
#include "ffx_denoiser_shadows/ffx_denoiser_shadows_filter.h"
#pragma dxc diagnostic pop

// g_Dispatch: x the pass (0 to 2), y its step in pixels. The first pass writes the next frame's
// history, the second writes back where the temporal pass wrote (whose tiles it skips hold their
// value there already), the third the shadow, with some of the contrast the blur took restored, as
// the denoiser's sample does.
[numthreads(8, 8, 1)]
void FilterShadows(uint2 gid : SV_GroupID, uint2 gtid : SV_GroupThreadID, uint2 did : SV_DispatchThreadID)
{
    s_DepthSigma = 1.0f + 0.05f * toLinearDepth(FFX_DNSR_Shadows_ReadDepth(did), g_Frame.nearFar.xy);

    const uint pass = g_Dispatch.x;
    bool write = false;
    float2 results = FFX_DNSR_Shadows_FilterSoftShadowsPass(gid, gtid, did, write, pass, g_Dispatch.y);
    if (!write || any(did >= g_Frame.dimensions.xy)) return;

    if (pass == 0) g_Shadow_History[did] = results;
    else if (pass == 1) g_Shadow_Reprojection[did] = results;
    else g_Shadow_Output[did] = pow(abs(results.x), max(1.2f - results.y, 1.0f));
}
