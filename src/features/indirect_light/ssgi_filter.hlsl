// The denoise half: an a-trous edge-aware filter, one dispatch per iteration, 5x5 taps whose
// stride doubles each step (1, 2, 4), weighted by depth, normal and the measured per-pixel noise.
// Pixels with no valid gather sample are skipped, not averaged in.
#include "ssgi_common.hlsli"
Texture2D<float4> Bounce : register(t0);   // The radiance to filter
Texture2D<float> Depth : register(t1);     // Reversed-Z, 0 on the sky
Texture2D<float4> Normals : register(t2);  // The G-buffer's view-space normals
Texture2D<float2> Moments : register(t3);  // x = mean luminance (0 means never sampled), y = mean-square
RWTexture2D<float4> Filtered : register(u0);

#define FILTER_MAX_RADIANCE 1.0e4

float luminanceOf(float3 colour)
{
    return dot(colour, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel = tid.xy;
    const float scale = max(Scale, 1.0);
    const int2 gs = (int2(Size) + (int)scale - 1) / (int)scale;
    if (any(pixel >= (uint2)gs)) return;

    // Depth and normals only exist at full resolution.
    const int2 centre = (int2)floor((float2(pixel) + 0.5) * scale);
    if (any(centre >= int2(Size))) return;

    const float4 centreSample = Bounce.Load(int3(pixel, 0));
    const float z = distanceOf(Depth.Load(int3(centre, 0)));
    if (!hasGeometry(z))
    {
        // Writing matters: a skipped write would leave the texel holding whatever it held.
        Filtered[pixel] = 0;
        return;
    }

    const float3 N = decodeNormal(Normals.Load(int3(centre, 0)).xyz);
    const float invZ = 1.0 / z;

    const float2 centreMoments = Moments.Load(int3(pixel, 0)).xy;
    const float centreLuminance = luminanceOf(centreSample.rgb);
    const float centreVariance = max(centreMoments.y - centreMoments.x * centreMoments.x, 0.0);
    // A floor on the noise estimate: on a clean surface the measured variance can be small enough
    // that the weight would reject every neighbour and nothing would be smoothed at all.
    const float sigmaLuminance = sqrt(centreVariance) + 0.02 + 0.1 * abs(centreLuminance);

    const int stride = 1 << (int)clamp(FilterStep, 0.0, 4.0);
    float3 sum = 0;
    float weightSum = 0;

    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            const int2 q = int2(pixel) + int2(dx, dy) * stride;
            if (any(q < 0) || any(q >= gs)) continue;

            // Never-sampled pixels carry zero radiance; averaging that in darkens neighbours.
            if (Moments.Load(int3(q, 0)).x <= 0.0) continue;

            const int2 full = clamp((int2)floor((float2(q) + 0.5) * scale), int2(0, 0), int2(Size) - 1);
            const float qz = distanceOf(Depth.Load(int3(full, 0)));
            if (!hasGeometry(qz)) continue;

            const float3 neighbour = Bounce.Load(int3(q, 0)).rgb;
            if (any(!isfinite(neighbour))) continue;

            const float normalMatch = saturate(dot(N, decodeNormal(Normals.Load(int3(full, 0)).xyz)));
            const float depthMatch = 1.0 / (1.0 + 32.0 * abs(qz - z) * invZ);
            const float luminanceMatch = 1.0 / (1.0 + abs(luminanceOf(neighbour) - centreLuminance) / sigmaLuminance);

            const float weight = normalMatch * normalMatch * normalMatch * normalMatch * depthMatch * luminanceMatch;
            sum += neighbour * weight;
            weightSum += weight;
        }
    }

    if (weightSum <= 1.0e-4)
    {
        Filtered[pixel] = centreSample;
        return;
    }
    // A near-zero weight sum divides the radiance up into the thousands; that value would live in
    // the history forever and render black, so both the division and the result are bounded.
    Filtered[pixel] = float4(clamp(sum / weightSum, 0.0, FILTER_MAX_RADIANCE), 1.0);
}
