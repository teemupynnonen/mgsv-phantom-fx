// The temporal half: reprojects with the game's velocity, rejects on depth, normal and screen
// bounds, clips the history to this frame's neighbourhood, keeps a ping-ponged history with its
// length in alpha, and maintains the luminance moments the denoiser reads.
#include "ssgi_common.hlsli"
#define HISTORY_MAX_LENGTH 32.0

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

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel = tid.xy;
    const float scale = max(Scale, 1.0);
    const int2 gs = (int2(Size) + (int)scale - 1) / (int)scale;
    if (any(pixel >= (uint2)gs)) return;

    const int2 centre = (int2)floor((float2(pixel) + 0.5) * scale);
    if (any(centre >= int2(Size))) return;

    const float4 current = Current.Load(int3(pixel, 0));
    const float z = distanceOf(Depth.Load(int3(centre, 0)));
    const float3 N = decodeNormal(Normals.Load(int3(centre, 0)).xyz);

    // The game's velocity, .ba packed as (v * 2 - 1) * 64 pixels, as UV here. Measured in the RenoDX
    // add-on: the TAA jitter cancels inside the buffer (the velocity pass runs with the jittered
    // projection; a still camera decodes to 0.03 px), so no jitter term is subtracted.
    const float2 velocity = (Velocity.Load(int3(centre, 0)).ba * 2.0 - 1.0) * 64.0 / float2(Size);

    // Reproject in gather space, then fetch the history with a manual bilinear tap.
    const float2 uv = (float2(centre) + 0.5) / float2(Size);
    const float2 previous = (uv - velocity) * float2(gs) - 0.5;
    const int2 base = (int2)floor(previous);
    const float2 fraction = frac(previous);
    float3 history = 0;
    float historyLength = 0;
    [unroll]
    for (int y = 0; y <= 1; ++y)
    {
        [unroll]
        for (int x = 0; x <= 1; ++x)
        {
            const int2 q = clamp(base + int2(x, y), int2(0, 0), gs - 1);
            const float4 tap = History.Load(int3(q, 0));
            const float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
            history += tap.rgb * weight;
            historyLength += tap.a * HISTORY_MAX_LENGTH * weight;
        }
    }

    // One non-finite value poisons the history permanently: the temporal blend carries it forward,
    // the moments square it into infinity, and the pixel renders black.
    history = all(isfinite(history)) ? clamp(history, 0.0, 1.0e4) : 0.0;
    const float3 gathered = all(isfinite(current.rgb)) ? clamp(current.rgb, 0.0, 1.0e4) : 0.0;

    const bool hasSample = current.a > 0.5;

    // Neighbourhood clip: the history may only move the result within what this frame's
    // neighbourhood allows. Starved neighbours are skipped, or the range always includes zero and
    // the history can only darken.
    float3 minimum = gathered;
    float3 maximum = gathered;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 q = clamp(int2(pixel) + int2(dx, dy), int2(0, 0), gs - 1);
            const float4 neighbour = Current.Load(int3(q, 0));
            if (neighbour.a <= 0.5 || any(!isfinite(neighbour.rgb))) continue;
            minimum = min(minimum, neighbour.rgb);
            maximum = max(maximum, neighbour.rgb);
        }
    }

    // Rejection against the geometry the history was accumulated on: a disocclusion or a
    // silhouette cannot be recognised from colour alone, which is why the aux target exists.
    const int2 auxPixel = clamp((int2)round(previous), int2(0, 0), gs - 1);
    const float4 previousAux = HistoryAux.Load(int3(auxPixel, 0));
    const float previousZ = previousAux.x;
    const float2 previousNormalXY = previousAux.yz;
    // Reconstruct z with this frame's sign instead of a blind +sqrt, which would mirror the normal
    // and reject everything as a geometry change.
    const float previousNormalZ = (N.z < 0.0 ? -1.0 : 1.0) * sqrt(saturate(1.0 - dot(previousNormalXY, previousNormalXY)));
    const float3 previousNormal = normalize(float3(previousNormalXY, previousNormalZ));
    const bool inside = previous.x >= 0.0 && previous.y >= 0.0 && previous.x <= (float)(gs.x - 1) && previous.y <= (float)(gs.y - 1);
    const bool depthMatches = previousZ > 0.0 && abs(z - previousZ) <= 0.02 * max(z, 1.0e-6);
    const bool normalMatches = dot(N, previousNormal) >= 0.9;
    const bool hasHistory = inside && depthMatches && normalMatches && historyLength > 0.5;

    // A freshly reset pixel trusts only the current sample and ramps back up; a converged one is
    // limited by the user's ceiling.
    const float adaptive = 1.0 - 1.0 / max(historyLength, 1.0);
    const float blend = hasHistory ? min(HistoryBlend, adaptive) * saturate(HistoryValid) : 0.0;
    const float3 result = hasSample ? lerp(gathered, clamp(history, minimum, maximum), blend) : history;

    const float len = hasSample ? (hasHistory ? min(historyLength + 1.0, HISTORY_MAX_LENGTH) : 1.0) : max(historyLength, 1.0);
    const float4 real = float4(result, len / HISTORY_MAX_LENGTH);

    // The history always receives the real result: a probe written into it would poison the next
    // frame and make the view self-fulfilling.
    HistoryOut[pixel] = real;
    HistoryAuxOut[pixel] = float4(z, N.x, N.y, len / HISTORY_MAX_LENGTH);

    // Luminance moments of the incoming sample, not of the blended result: the variance has to
    // describe how much the gather itself fluctuates, or the denoiser gets no signal about where
    // the noise is.
    const float sampleLuminance = dot(gathered, float3(0.2126, 0.7152, 0.0722));
    const float2 previousMoments = Moments.Load(int3(auxPixel, 0)).xy;
    float m1 = previousMoments.x;
    float m2 = previousMoments.y;
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
        // Reprojection offset in pixels: R/G the jitter the scene constants advertise, B the raw
        // velocity's magnitude.
        Accumulated[pixel] = float4(Scene[28].x * (float)Size.x, Scene[28].y * (float)Size.y,
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
