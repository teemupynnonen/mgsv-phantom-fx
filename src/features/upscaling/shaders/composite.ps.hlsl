// Writes the upscaled output, sharpened with AMD FidelityFX RCAS (FsrRcasF in ffx_fsr1.h, MIT:
// AMD-FidelityFX-LICENSE.txt), whose noise filter is left out. RCAS expects perceptual values in 0 to
// 1, which the sRGB-encoded LDR colour is. Alpha is 0; the grading writes its own.
#include "../../../shaders/hdr_common.hlsli"

cbuffer CompositeParameters : register(b0)
{
    // In place of the image: the motion vectors, or red where the engine's velocity was used.
    uint ShowMotion;
    uint ShowObjectMotion;
    uint ShowProbe; // The probes' cross over the image
    float Sharpness;
    uint ObjectMotion;
    uint Linear; // The upscaler made linear colour, sharpened through x / (1 + x) and encoded here
    uint2 Padding;
};
Texture2D<float4> Upscaled : register(t0);
Texture2D<float2> Motion : register(t1);
Texture2D<float4> Velocity : register(t2);
// A debug view over the image, at the render size, by its alpha (ray_tracing::shadowDebug or lightDebug);
// unbound while none shows.
Texture2D<float4> ShadowRays : register(t3);

float3 Load(int2 pixel)
{
    uint width, height;
    Upscaled.GetDimensions(width, height);
    const float3 color = Upscaled.Load(int3(clamp(pixel, 0, int2(width, height) - 1), 0)).rgb;
    // RCAS takes values in 0 to 1; light maps there through x / (1 + x), and back after.
    return Linear ? max(color, 0) / (1 + max(color, 0)) : saturate(color);
}

// b, d, f and h are the pixels above, left, right and below e.
float3 Rcas(float3 b, float3 d, float3 e, float3 f, float3 h)
{
    float3 low = min(min(b, d), min(f, h)), high = max(max(b, d), max(f, h));
    // The lobe at which each channel would clip below 0 or above 1.
    float3 hitMin = min(low, e) / max(4 * high, 1.0 / 65536);
    float3 hitMax = (1 - max(high, e)) / min(4 * low - 4, -1.0 / 65536);
    float3 lobes = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16), min(max(lobes.r, max(lobes.g, lobes.b)), 0)) * Sharpness;
    return (lobe * (b + d + f + h) + e) / (4 * lobe + 1);
}

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float3 color = Load(pixel);
    if (ShowMotion)
    {
        float2 motion = Motion.Load(int3(pixel, 0));
        color = float3(saturate(0.5 + motion * 0.05), saturate(length(motion) * 0.05));
    }
    else if (ShowObjectMotion)
    {
        bool used = ObjectMotion && Velocity.Load(int3(pixel, 0)).r > 0.5;
        color = lerp(color * 0.5, float3(1, 0, 0), used ? 0.6 : 0);
    }
    else if (Sharpness > 0)
        color = Rcas(Load(pixel + int2(0, -1)), Load(pixel + int2(-1, 0)), color, Load(pixel + int2(1, 0)), Load(pixel + int2(0, 1)));

    if (Linear && !ShowMotion && !ShowObjectMotion) color = LinearToSrgb(color / max(1 - color, 1.0 / HalfMax));

    uint width, height, raysWidth, raysHeight;
    Upscaled.GetDimensions(width, height);
    ShadowRays.GetDimensions(raysWidth, raysHeight);
    if (raysWidth)
    {
        float4 rays = ShadowRays.Load(int3(position.xy * float2(raysWidth, raysHeight) / float2(width, height), 0));
        color = lerp(color, rays.rgb, rays.a);
    }

    if (ShowProbe)
    {
        // The probes' pixel, the screen's centre, sun or none: indoors only the lamps' probe runs, and
        // without a sun the rays' view stays empty.
        int2 fromCentre = int2(position.xy) - int2(uint2(width, height) / 2);
        bool onCross = (abs(fromCentre.x) <= 1 && abs(fromCentre.y) <= 18) || (abs(fromCentre.y) <= 1 && abs(fromCentre.x) <= 18);
        if (onCross) color = 1;
    }

    return float4(color, 0);
}
