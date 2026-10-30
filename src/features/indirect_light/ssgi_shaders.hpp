#pragma once

// HLSL for SSGI (ssgi.hpp), compiled at runtime with d3dcompiler_47. The chain is ported from the
// RenoDX MGSV add-on's indirect lighting (renodx-mgsv src/games/mgsv/il): a golden-angle gather of
// the light the neighbouring surfaces reflect, temporal accumulation against the game's velocity
// buffer, an a-trous denoiser driven by measured luminance moments, and a depth-joint bilateral
// resolve into the diffuse light the game's resolve reads.
namespace phantom_fx::ssgi::shaders
{
// The constant buffer of the passes, b0; b1 is the game's cPSScene, as the visibility bitmask
// march's shaders read it (docs/DEVELOPMENT.md, "Constant buffers").
#define PHANTOM_FX_SSGI_PARAMETERS R"(
cbuffer GI : register(b0)
{
    uint2 Size;          // The render size
    float Radius;        // Share of the screen's height the gather reaches
    float Amount;        // The gather's multiplier at the resolve
    float Samples;       // Gather samples per pixel
    float Scale;         // The gather's resolution divisor: 1, 2 or 4
    float FilterStep;    // The a-trous iteration: 0, 1 or 2
    float HistoryBlend;  // Ceiling on the history blend
    float HistoryValid;  // 0 when the velocity is missing or the history has reset
    float Debug;         // Diagnostic view, 0 off
    float DiffuseScale;  // 0 shows the gather alone (the diagnostics)
    float Random;        // This frame's salt for the gather's rotation
    float MinSamples;    // Samples that must survive rejection
    float Visibility;    // Ray-march steps per tap for the occlusion test, 0 disables it
    float Padding1;      // Rounds the buffer to 64 bytes
    float Padding2;
};

cbuffer Scene : register(b1)
{
    float4 Scene[30];
};

// The linear view distance of a reversed-Z depth sample; 0 on the sky.
float distanceOf(float depth)
{
    return depth > 0 ? Scene[21].z / (depth - Scene[21].w) : 0;
}

bool hasGeometry(float distance)
{
    return distance > 0 && distance < 1e6;
}

// The view-space position of a pixel, as the visibility bitmask march computes it: Fox Engine's
// view space looks down +z and mirrors x (P00 < 0), flipped here to the usual -z, x right.
float3 viewPosition(float2 pixelCentre, float distance)
{
    float2 ndc = pixelCentre / Size * float2(2, -2) + float2(-1, 1) - Scene[28].xy;
    return float3(ndc * float2(abs(Scene[21].x), Scene[21].y), -1) * distance;
}

// DecodeViewSpaceNormal, as the game's own ambient obscurance reads the G-buffer.
float3 decodeNormal(float3 encoded)
{
    float z = encoded.z * encoded.z * 2 - 1;
    float2 xy = encoded.xy * 2 - 1;
    xy *= sqrt(saturate(1 - z * z)) * rsqrt(max(dot(xy, xy), 1e-8));
    return normalize(float3(sign(Scene[21].x) * xy.x, xy.y, -z));
}

// Integer hash for the gather's rotation: interleaved gradient noise is a diagonal ramp, which
// read as screen-space stripes, and a hash has no screen-space structure to leak.
uint hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hash01(uint2 pixel, uint salt)
{
    return (float)(hash(pixel.x + hash(pixel.y + hash(salt))) & 0xFFFFFFu) * (1.0 / 16777216.0);
}

// Golden-angle spiral over the unit disk with a jittered radius, so no fixed set of radii bands.
float2 diskSample(uint index, uint count, float rotation, float radiusJitter)
{
    const float radius = sqrt((index + radiusJitter) / (float)count);
    const float angle = rotation + index * 2.39996323;
    return radius * float2(cos(angle), sin(angle));
}
)"

// The gather: up to 16 taps around each pixel at the gather's resolution, each rejected or
// weighted by plane elevation, source facing and distance, returning the mean radiance
// (albedo x diffuse light + specular) of the taps that survive. Alpha is validity. It reads the
// game's diffuse light and writes its own target, so reading neighbours while writing this pixel
// is no hazard.
inline constexpr char Gather[] = PHANTOM_FX_SSGI_PARAMETERS R"(
Texture2D<float> Depth : register(t0);        // Reversed-Z, 0 on the sky
Texture2D<float4> Normals : register(t1);     // The G-buffer's view-space normals
Texture2D<float4> LightSource : register(t2); // The game's diffuse light, as the resolve reads it
Texture2D<float4> Albedo : register(t3);      // Through the resolve's sRGB view, so linear
Texture2D<float4> Specular : register(t4);    // The specular light the resolve adds (its inLightSpecular)
// The prefiltered distances and their smaller copies (visibility_bitmask's prefilter), negative on
// foliage; the per-tap occlusion test marches them.
Texture2D<float> Distances : register(t5);
RWTexture2D<float4> Bounce : register(u0);

#define SSGI_MAX_SAMPLES 16

static const uint DistanceMipCount = 5;
static const float DistanceMipOffset = 3.3;
static const uint VisibilityStepLimit = 8;

// The distance at a sample `offset` pixels from the receiver, from a smaller copy the farther it
// lies; negative on foliage.
float signedDistanceAt(int2 texel, float offset)
{
    uint mip = uint(round(clamp(log2(max(offset, 1.0)) - DistanceMipOffset, 0, DistanceMipCount - 1)));
    uint2 mipSize = max(Size >> mip, 1);
    return Distances.Load(int3(min(uint2(max(texel, int2(0, 0))) >> mip, mipSize - 1), mip));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel = tid.xy;
    const float scale = max(Scale, 1.0);
    const int2 gs = (int2(Size) + (int)scale - 1) / (int)scale;
    if (any(pixel >= (uint2)gs)) return;

    // The gather may run below full resolution; its threads map onto full-resolution pixels, which
    // is where every G-buffer read and every sample offset lives.
    const int2 centre = (int2)floor((float2(pixel) + 0.5) * scale);
    if (any(centre >= int2(Size))) return;

    const float z = distanceOf(Depth.Load(int3(centre, 0)));
    if (!hasGeometry(z))
    {
        Bounce[pixel] = 0;
        return;
    }

    const float3 P = viewPosition(float2(centre), z);
    const float3 N = decodeNormal(Normals.Load(int3(centre, 0)).xyz);

    // The radius as a share of the screen's height, and the same radius in view units at this
    // depth: a world-space radius needs a known world scale, and the distance is not in metres.
    const float radiusFraction = max(Radius, 0.001);
    const float radiusPixels = radiusFraction * Size.y;
    const float radiusWorld = radiusFraction * 2.0 * z * Scene[21].y;

    const uint sampleCount = (uint)clamp(Samples, 1.0, (float)SSGI_MAX_SAMPLES);
    const uint minSamples = (uint)clamp(MinSamples, 1.0, (float)SSGI_MAX_SAMPLES);
    const uint frameSalt = (uint)(Random * 65535.0);
    const float rotation = hash01(pixel, frameSalt) * 6.2831853;

    float3 sum = 0;
    float weightSum = 0;
    uint validSamples = 0;

    [loop]
    for (uint index = 0; index < sampleCount; ++index)
    {
        const float radiusJitter = hash01(pixel, index * 977u + frameSalt + 1u);
        const int2 samplePixel = (int2)round(float2(centre) + diskSample(index, sampleCount, rotation, radiusJitter) * radiusPixels);
        if (any(samplePixel < 0) || any(samplePixel >= int2(Size))) continue;

        const float sampleZ = distanceOf(Depth.Load(int3(samplePixel, 0)));
        if (!hasGeometry(sampleZ)) continue;

        const float3 S = viewPosition(float2(samplePixel), sampleZ);
        const float3 delta = S - P;
        const float distanceSq = dot(delta, delta);
        if (distanceSq < 1e-8) continue;

        const float distance = sqrt(distanceSq);
        const float3 direction = delta / distance;

        // Visibility: march the tap's ray over the prefiltered distances; a surface nearer than the
        // ray blocks the sample. The disk alone would not see an occluder and would bleed its light
        // through.
        const uint visibilitySteps = (uint)clamp(Visibility, 0.0, (float)VisibilityStepLimit);
        if (visibilitySteps > 0)
        {
            const float2 rayPixels = float2(samplePixel) - float2(centre);
            const float pixelLength = length(rayPixels);
            bool blocked = false;
            [loop]
            for (uint k = 0; k < visibilitySteps; ++k)
            {
                const float f = (k + 0.5) / visibilitySteps;
                const int2 q = int2(round(float2(centre) + rayPixels * f));
                const float surface = abs(signedDistanceAt(q, f * pixelLength));
                const float ray = length(P + direction * (f * distance));
                const float thickness = clamp(0.12 * ray, 0.02, max(radiusWorld, 0.02));
                if (surface < ray - thickness)
                {
                    blocked = true;
                    break;
                }
            }
            if (blocked) continue;
        }

        // A sample on the same surface is the pixel's own light: adding it back is what makes a
        // gather read as a halo rather than as bounce. The soft ramp is the cosine term too.
        const float planeWeight = smoothstep(0.02, 0.35, dot(N, direction));
        if (planeWeight <= 0) continue;

        const float sourceFacing = dot(decodeNormal(Normals.Load(int3(samplePixel, 0)).xyz), -direction);
        if (sourceFacing <= 0) continue;

        // A fixed inverse-square against the radius; the gather returns a normalized mean, so the
        // kernel's shape cancels wherever the neighbourhood is uniform.
        const float range = 1.0 + 4.0 * distanceSq / (radiusWorld * radiusWorld);
        const float weight = planeWeight * sourceFacing / range;

        // Source radiance: albedo times diffuse light turns the gather into colour bleeding, and
        // the specular is what lets a light fixture contribute more than a dim wall.
        const float3 radiance = Albedo.Load(int3(samplePixel, 0)).rgb * LightSource.Load(int3(samplePixel, 0)).rgb
                              + Specular.Load(int3(samplePixel, 0)).rgb;
        sum += radiance * weight;
        weightSum += weight;
        ++validSamples;
    }

    const bool valid = validSamples >= minSamples;
    Bounce[pixel] = float4(valid ? sum / max(weightSum, 1e-6) : 0.0, valid ? 1.0 : 0.0);
}
)";

// The temporal half: reprojects with the game's velocity, rejects on depth, normal and screen
// bounds, clips the history to this frame's neighbourhood, keeps a ping-ponged history with its
// length in alpha, and maintains the luminance moments the denoiser reads.
inline constexpr char Accumulate[] = PHANTOM_FX_SSGI_PARAMETERS R"(
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
)";

// The denoise half: an a-trous edge-aware filter, one dispatch per iteration, 5x5 taps whose
// stride doubles each step (1, 2, 4), weighted by depth, normal and the measured per-pixel noise.
// Pixels with no valid gather sample are skipped, not averaged in.
inline constexpr char Filter[] = PHANTOM_FX_SSGI_PARAMETERS R"(
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
)";

// The full-resolution resolve: a depth-joint bilateral upsample of the filtered gather, scaled by
// Amount and added to the game's diffuse light. A plain bilinear would pull the background's
// bounce onto the foreground across a silhouette, which reads as a halo exactly where the eye is.
inline constexpr char Resolve[] = PHANTOM_FX_SSGI_PARAMETERS R"(
Texture2D<float4> Filtered : register(t0); // At the gather's size
Texture2D<float> Depth : register(t1);     // Reversed-Z, 0 on the sky
Texture2D<float4> Diffuse : register(t2);  // The game's diffuse light, as the resolve reads it
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel = tid.xy;
    if (any(pixel >= Size)) return;

    const float scale = max(Scale, 1.0);
    const int2 gs = (int2(Size) + (int)scale - 1) / (int)scale;
    const float2 position = (float2(pixel) + 0.5) / scale - 0.5;
    const int2 base = (int2)floor(position);
    const float2 fraction = frac(position);

    const float z = distanceOf(Depth.Load(int3(pixel, 0)));
    const bool geometry = hasGeometry(z);
    const float invZ = geometry ? 1.0 / z : 0.0;

    float3 radiance = 0;
    float weightSum = 0;
    [unroll]
    for (int y = 0; y <= 1; ++y)
    {
        [unroll]
        for (int x = 0; x <= 1; ++x)
        {
            const int2 q = clamp(base + int2(x, y), int2(0, 0), gs - 1);
            const float4 tap = Filtered.Load(int3(q, 0));
            const float bilinear = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);

            const int2 full = clamp((int2)floor((float2(q) + 0.5) * scale), int2(0, 0), int2(Size) - 1);
            const float qz = distanceOf(Depth.Load(int3(full, 0)));
            const float depthMatch = (geometry && qz > 0.0) ? 1.0 / (1.0 + 32.0 * abs(qz - z) * invZ) : 1.0;

            const float weight = bilinear * depthMatch;
            radiance += tap.rgb * weight;
            weightSum += weight;
        }
    }

    const float3 bounce = radiance / max(weightSum, 1.0e-4) * Amount;
    const float4 diffuse = Diffuse.Load(int3(pixel, 0));
    const float3 output = Debug > 0.5 ? bounce : diffuse.rgb * DiffuseScale + bounce;
    Out[pixel] = float4(output, diffuse.a);
}
)";
}  // namespace phantom_fx::ssgi::shaders
