// The constants and helpers the VB-SSGI passes share (ssgi.hpp), included by each pass's file.
// HLSL for VB-SSGI (ssgi.hpp), compiled at runtime with d3dcompiler_47. The chain is ported from the
// RenoDX MGSV add-on's indirect lighting (renodx-mgsv src/games/mgsv/il): a golden-angle gather of
// the light the neighbouring surfaces reflect, temporal accumulation against the game's velocity
// buffer, an a-trous denoiser driven by measured luminance moments, and a depth-joint bilateral
// resolve into the diffuse light the game's resolve reads.
// The constant buffer of the passes, b0; b1 is the game's cPSScene, as the visibility bitmask
// march's shaders read it (docs/DEVELOPMENT.md, "Constant buffers").
cbuffer GI : register(b0)
{
    uint2 Size;          // The render size
    float Radius;        // Metres the gather reaches around a pixel
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
