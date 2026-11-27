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

// B3-spline a-trous kernel; the weights of a uniform box ring and leak across the step
static const float KERNEL[5] = { 1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0 };

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

    // Pre-blur of the variance: one pixel's estimate comes from few frames and is itself noisy, and
    // a noisy sigma makes the edge-stopping flicker. A 3x3 tent over the sampled neighbours.
    float varianceSum = 0;
    float varianceWeight = 0;
    [unroll]
    for (int vy = -1; vy <= 1; ++vy)
    {
        [unroll]
        for (int vx = -1; vx <= 1; ++vx)
        {
            const int2 q = int2(pixel) + int2(vx, vy);
            if (any(q < 0) || any(q >= gs)) continue;
            const float2 m = Moments.Load(int3(q, 0)).xy;
            if (m.x <= 0.0) continue;
            const float w = (vx == 0 ? 2.0 : 1.0) * (vy == 0 ? 2.0 : 1.0);
            varianceSum += max(m.y - m.x * m.x, 0.0) * w;
            varianceWeight += w;
        }
    }
    const float centreLuminance = luminanceOf(centreSample.rgb);
    const float centreVariance = varianceWeight > 0.0 ? varianceSum / varianceWeight : 0.0;
    // A floor on the noise estimate: on a clean surface the measured variance can be small enough
    // that the weight would reject every neighbour and nothing would be smoothed at all.
    const float sigmaLuminance = sqrt(centreVariance) + 0.02 + 0.1 * abs(centreLuminance);

    // The depth's local gradient, from whichever one-sided difference is flatter so a silhouette
    // does not inflate it: a tilted surface's neighbours differ in depth by that gradient times
    // the offset, which is not an edge.
    float2 depthGradient = 0;
    {
        const int2 step = int2(scale, scale);
        const int2 lo = max(centre - step, int2(0, 0));
        const int2 hi = min(centre + step, int2(Size) - 1);
        const float zx0 = distanceOf(Depth.Load(int3(lo.x, centre.y, 0)));
        const float zx1 = distanceOf(Depth.Load(int3(hi.x, centre.y, 0)));
        const float zy0 = distanceOf(Depth.Load(int3(centre.x, lo.y, 0)));
        const float zy1 = distanceOf(Depth.Load(int3(centre.x, hi.y, 0)));
        const float2 backward = float2(hasGeometry(zx0) ? z - zx0 : 0.0, hasGeometry(zy0) ? z - zy0 : 0.0);
        const float2 forward = float2(hasGeometry(zx1) ? zx1 - z : 0.0, hasGeometry(zy1) ? zy1 - z : 0.0);
        depthGradient = float2(abs(backward.x) < abs(forward.x) ? backward.x : forward.x,
                               abs(backward.y) < abs(forward.y) ? backward.y : forward.y) / (float)scale;
    }
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
            // Tolerance grows with the gradient times the full-resolution offset; the relative floor
            // keeps flat surfaces strict.
            const float expected = abs(dot(depthGradient, float2(dx, dy) * (float)(stride * (int)scale)));
            const float depthMatch = exp(-abs(qz - z) / (expected + 0.01 * z));
            const float luminanceMatch = 1.0 / (1.0 + abs(luminanceOf(neighbour) - centreLuminance) / sigmaLuminance);

            const float weight = KERNEL[dx + 2] * KERNEL[dy + 2] * normalMatch * normalMatch * normalMatch * normalMatch * depthMatch * luminanceMatch;
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
