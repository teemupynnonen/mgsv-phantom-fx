// The temporal half: reprojects with the game's velocity, validates each history tap on plane distance, normal
// and screen bounds, variance-clips the history to this frame's neighbourhood, keeps a ping-ponged history with its
// length in alpha, and maintains the luminance moments the denoiser reads.
#include "ssgi_common.hlsli"
#define HISTORY_MAX_LENGTH 32.0
#define VARIANCE_CLIP_GAMMA 1.5

Texture2D<float4> Current : register(t0);      // This frame's gather
Texture2D<float4> History : register(t1);      // The previous accumulated result
Texture2D<float4> Velocity : register(t2);     // The game's velocity, .ba
Texture2D<float> Depth : register(t3);         // Reversed-Z, 0 on the sky
Texture2D<float4> Normals : register(t4);      // The G-buffer's view-space normals
Texture2D<float4> HistoryAux : register(t5);   // Previous frame's depth, normal.xy, length
Texture2D<float2> Moments : register(t6);      // Running luminance moments: mean, mean-square
RWTexture2D<float4> HistoryOut : register(u0);
RWTexture2D<float4> Accumulated : register(u1); // What the filter reads; a debug view may show here
RWTexture2D<float4> HistoryAuxOut : register(u2);
RWTexture2D<float2> MomentsOut : register(u3);

float3 toYCoCg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 fromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel = tid.xy;
    const float scale = max(Scale, 1.0);
    const int2 gs = (int2(Size) + (int)scale - 1) / (int)scale;
    if (any(pixel >= (uint2)gs)) return;

    // A partial block at the right or bottom edge still owns its pixel: sample the last valid one.
    const int2 centre = min((int2)floor((float2(pixel) + 0.5) * scale), int2(Size) - 1);

    const float4 current = Current.Load(int3(pixel, 0));
    const float z = distanceOf(Depth.Load(int3(centre, 0)));
    const float3 N = decodeNormal(Normals.Load(int3(centre, 0)).xyz);

    // The game's velocity, .ba packed as (v * 2 - 1) * 64 pixels, as UV here. Measured in the RenoDX
    // add-on: the TAA jitter cancels inside the buffer (the velocity pass runs with the jittered
    // projection; a still camera decodes to 0.03 px), so no jitter term is subtracted.
    const float2 velocity = (Velocity.Load(int3(centre, 0)).ba * 2.0 - 1.0) * 64.0 / float2(Size);

    // Reproject in gather space, then fetch the history with a manual bilinear tap.
    // From the gather texel's own block centre, not the clamped full-resolution pixel: with zero
    // velocity this lands exactly on the texel, so a still image is not bilinearly blurred.
    const float2 uv = (float2(pixel) + 0.5) * scale / float2(Size);
    const float2 previous = (uv - velocity) * float2(Size) / scale - 0.5;
    const int2 base = (int2)floor(previous);
    const float2 fraction = frac(previous);
    const bool inside = previous.x >= 0.0 && previous.y >= 0.0 && previous.x <= (float)(gs.x - 1) && previous.y <= (float)(gs.y - 1);

    // Each tap is validated on its own against the geometry it was accumulated on, and the weights
    // are renormalised over the valid ones: one test on the blended fetch lets a silhouette tap
    // bleed the wrong surface's light in, or throws away the three taps that were fine. The aux
    // target is what makes this possible, a disocclusion cannot be told from colour alone.
    const float3 currentPosition = viewPosition(float2(centre) + 0.5, z);
    float3 history = 0;
    float2 previousMoments = 0;
    float historyLength = 0;
    float validWeight = 0;
    [unroll]
    for (int y = 0; y <= 1; ++y)
    {
        [unroll]
        for (int x = 0; x <= 1; ++x)
        {
            const int2 q = clamp(base + int2(x, y), int2(0, 0), gs - 1);
            const float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
            const float4 tapAux = HistoryAux.Load(int3(q, 0));
            // The depth's sign carries the sign of the normal's z (written below), so the previous
            // normal is rebuilt with its own sign, not this frame's.
            const float tapZ = abs(tapAux.x);
            const float tapNormalZ = (tapAux.x < 0.0 ? -1.0 : 1.0) * sqrt(saturate(1.0 - dot(tapAux.yz, tapAux.yz)));
            const float3 tapNormal = normalize(float3(tapAux.yz, tapNormalZ));

            // Distance of the tap's surface point to this pixel's tangent plane: a relative depth
            // test fails on grazing surfaces, where neighbouring pixels legitimately differ in depth,
            // and passes a thin step at the same distance.
            const int2 tapFull = min((int2)floor((float2(q) + 0.5) * scale), int2(Size) - 1);
            const float3 tapPosition = viewPosition(float2(tapFull) + 0.5, tapZ);
            const float planeDistance = abs(dot(N, tapPosition - currentPosition));
            const bool valid = tapZ > 0.0 && planeDistance <= 0.01 * z && dot(N, tapNormal) >= 0.9 && tapAux.w > 0.0;
            if (!valid) continue;

            const float4 tap = History.Load(int3(q, 0));
            history += tap.rgb * weight;
            historyLength += tap.a * HISTORY_MAX_LENGTH * weight;
            previousMoments += Moments.Load(int3(q, 0)).xy * weight;
            validWeight += weight;
        }
    }
    const bool tapsValid = inside && validWeight > 1.0e-3;
    const float invValid = tapsValid ? 1.0 / validWeight : 0.0;
    history *= invValid;
    historyLength *= invValid;
    previousMoments *= invValid;

    // One non-finite value poisons the history permanently: the temporal blend carries it forward,
    // the moments square it into infinity, and the pixel renders black.
    history = all(isfinite(history)) ? clamp(history, 0.0, 1.0e4) : 0.0;
    const float3 gathered = all(isfinite(current.rgb)) ? clamp(current.rgb, 0.0, 1.0e4) : 0.0;

    const bool hasSample = current.a > 0.5;

    // Variance clipping: the history may only move the result within mean +- gamma * sigma of this
    // frame's 3x3, in YCoCg where luma and chroma separate. A min/max box is as wide as the
    // noisiest outlier, so it lets a ghost through exactly where the gather is noisy. Starved
    // neighbours are skipped, or the statistics always include zero and the history can only darken.
    float3 moment1 = 0;
    float3 moment2 = 0;
    float count = 0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 q = clamp(int2(pixel) + int2(dx, dy), int2(0, 0), gs - 1);
            const float4 neighbour = Current.Load(int3(q, 0));
            if (neighbour.a <= 0.5 || any(!isfinite(neighbour.rgb))) continue;
            const float3 ycocg = toYCoCg(min(neighbour.rgb, 1.0e4));
            moment1 += ycocg;
            moment2 += ycocg * ycocg;
            count += 1.0;
        }
    }
    float3 clippedHistory = history;
    if (count > 0.5)
    {
        const float3 mean = moment1 / count;
        const float3 sigma = sqrt(max(moment2 / count - mean * mean, 0.0));
        const float3 extent = VARIANCE_CLIP_GAMMA * sigma + 1.0e-4;
        const float3 offset = toYCoCg(history) - mean;
        // Scale the offset to the box surface along the line to the mean, not per channel, so the
        // history's hue is kept.
        const float3 normalised = abs(offset) / extent;
        const float outside = max(normalised.x, max(normalised.y, normalised.z));
        clippedHistory = fromYCoCg(mean + offset / max(outside, 1.0));
    }

    // One decision for colour, length, moments and the diagnostic: the history is usable or it is not.
    const bool hasHistory = tapsValid && historyLength > 0.5;
    // A freshly reset pixel trusts only the current sample and ramps back up; a converged one is
    // limited by the user's ceiling.
    const float adaptive = 1.0 - 1.0 / max(historyLength, 1.0);
    const float blend = hasHistory ? min(HistoryBlend, adaptive) * saturate(HistoryValid) : 0.0;
    // No sample and no history leaves an invalid pixel (length 0); no sample over valid history keeps it.
    const float3 result = hasSample ? lerp(gathered, clippedHistory, blend) : (hasHistory ? history : 0.0);

    const float len = hasSample ? (hasHistory ? min(historyLength + 1.0, HISTORY_MAX_LENGTH) : 1.0)
                                : (hasHistory ? min(historyLength, HISTORY_MAX_LENGTH) : 0.0);
    const float4 real = float4(result, len / HISTORY_MAX_LENGTH);

    // The history always receives the real result: a probe written into it would poison the next
    // frame and make the view self-fulfilling.
    HistoryOut[pixel] = real;
    HistoryAuxOut[pixel] = float4(N.z < 0.0 ? -z : z, N.x, N.y, len / HISTORY_MAX_LENGTH);

    // Luminance moments of the incoming sample, not of the blended result: the variance has to
    // describe how much the gather itself fluctuates, or the denoiser gets no signal about where
    // the noise is.
    const float sampleLuminance = dot(gathered, float3(0.2126, 0.7152, 0.0722));
    float m1 = hasHistory ? previousMoments.x : 0.0;
    float m2 = hasHistory ? previousMoments.y : 0.0;
    if (current.a > 0.5)
    {
        if (hasHistory)
        {
            m1 = lerp(sampleLuminance, m1, blend);
            m2 = lerp(sampleLuminance * sampleLuminance, m2, blend);
        }
        else
        {
            m1 = sampleLuminance;
            m2 = sampleLuminance * sampleLuminance;
        }
    }
    const float variance = all(isfinite(float2(m1, m2))) ? max(m2 - m1 * m1, 0.0) : 0.0;
    const float relativeVariance = variance / max(m1 * m1, 1.0e-6);
    MomentsOut[pixel] = float2(m1, m2);

    if (Debug > 4.5)
    {
        // Variance: black is a clean average, mid grey a 100% relative spread.
        const float display = relativeVariance / (1.0 + relativeVariance);
        Accumulated[pixel] = float4(display, 0.0, 0.0, 1.0);
    }
    else if (Debug > 3.5)
    {
        // Reprojection offset in pixels: R/G the jitter the scene constants advertise, shifted by
        // half a pixel so mid grey is none (the display is unsigned and would clip the negative
        // half), B the raw velocity's magnitude.
        Accumulated[pixel] = float4(Scene[28].x * (float)Size.x + 0.5, Scene[28].y * (float)Size.y + 0.5,
                                    length(abs(velocity) * float2(Size)), 1.0);
    }
    else if (Debug > 2.5)
    {
        // History length: dark where the history was just reset, bright where it accumulated.
        Accumulated[pixel] = float4(len / HISTORY_MAX_LENGTH, hasHistory ? 0.0 : 1.0, 0.0, 1.0);
    }
    else if (Debug > 1.5)
    {
        // History delta: must be near-black with a static camera.
        Accumulated[pixel] = float4(abs(history - current.rgb) * 10.0, 1.0);
    }
    else
    {
        Accumulated[pixel] = real;
    }
}
