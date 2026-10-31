#pragma once

// HLSL for the forward surfaces' shadows (forward_shadows.hpp), compiled at runtime with d3dcompiler_47.
namespace phantom_fx::forward_shadows::shaders
{
// The rays' reflection where the surface drew: added to the water in place of the game's cube map, which the
// draw binds black. What the surface reflects is the scene's light, not the sun's alone, so the rays' shadow
// does not scale it: a shaded river still mirrors what is around it. The trace carries the surface's own
// specular albedo (its forward guides, one frame old), so no gain is needed here.
inline constexpr char Reflection[] = R"(
cbuffer Parameters : register(b0)
{
    row_major float4x4 viewProjection;         // This frame's, the one the water is drawn with
    row_major float4x4 viewProjectionInverse;
    row_major float4x4 previousViewProjection; // The one the guides were drawn with, a frame ago
    row_major float4x4 previousViewProjectionInverse;
    float4 eye;
    float2 dimensions;
    float2 padding;
};
Texture2D<float4> Reflected : register(t0); // The traced reflection, at last frame's pixels
Texture2D<float> GuideDepth : register(t1); // The forward surface's depth, last frame
Texture2D<float> GuideMask : register(t2);  // Where it drew
SamplerState Linear : register(s0);

float3 Unproject(float2 uv, float depth, float4x4 transform)
{
    const float4 clip = mul(transform, float4(2 * float2(uv.x, 1 - uv.y) - 1, depth, 1));
    return clip.xyz / clip.w;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 pixel = int2(position.xy);
    const float4 centre = Reflected.Load(int3(pixel, 0));
    const float mask = GuideMask.Load(int3(pixel, 0));
    const float depth = GuideDepth.Load(int3(pixel, 0));
    if (mask <= 0.0f || depth <= 0.0f) return float4(centre.rgb, 0.0f);

    // The guide is a frame old: the pixel holds the water point that was here last frame. What this frame's
    // ray meets is that point moved by the camera - the water is a plane through it, along its slope.
    const float2 uv = (pixel + 0.5f) / dimensions;
    const float3 previousPoint = Unproject(uv, depth, previousViewProjectionInverse);
    const int2 last = int2(dimensions) - 1;
    const int2 rightPixel = min(pixel + int2(1, 0), last);
    const int2 downPixel = min(pixel + int2(0, 1), last);
    float3 normal = float3(0, 0, -1);
    if (GuideMask.Load(int3(rightPixel, 0)) > 0.0f && GuideMask.Load(int3(downPixel, 0)) > 0.0f)
    {
        const float3 right = Unproject((rightPixel + 0.5f) / dimensions, GuideDepth.Load(int3(rightPixel, 0)), previousViewProjectionInverse);
        const float3 down = Unproject((downPixel + 0.5f) / dimensions, GuideDepth.Load(int3(downPixel, 0)), previousViewProjectionInverse);
        const float3 slope = cross(right - previousPoint, down - previousPoint);
        if (!all(slope == 0.0f)) normal = normalize(slope);
    }
    if (dot(normal, eye.xyz - previousPoint) < 0.0f) normal = -normal;

    const float3 onRay = Unproject(uv, 0.5f, viewProjectionInverse);
    const float3 direction = normalize(onRay - eye.xyz);
    const float denominator = dot(direction, normal);
    float3 current = previousPoint;
    if (abs(denominator) > 1e-4f) current = eye.xyz + direction * (dot(previousPoint - eye.xyz, normal) / denominator);

    // Where that point was on the screen last frame: its reflection is there.
    const float4 previousClip = mul(previousViewProjection, float4(current, 1));
    if (previousClip.w <= 0.0f) return float4(centre.rgb, 0.0f);
    const float2 previousUv = previousClip.xy / previousClip.w * float2(0.5f, -0.5f) + 0.5f;
    if (any(previousUv < 0.0f) || any(previousUv > 1.0f)) return float4(centre.rgb, 0.0f);

    return float4(Reflected.SampleLevel(Linear, previousUv, 0).rgb, 0.0f);
}
)";

// The forward surface's guides, for the reflections traced at its pixels (`ray_tracing`'s TraceReflections):
// the pixel's depth, and the surface's kind in the mask (1 a river or a liquid, 0.5 glass). The normal and
// the roughness are worked out of the depth at the trace.
inline constexpr char Guide[] = R"(
cbuffer Parameters : register(b0)
{
    float surface;
    float3 padding;
};

struct Input
{
    float4 position : SV_Position;
};
struct Output
{
    float depth : SV_Target0;
    float surface : SV_Target1;
};

Output main(Input input)
{
    Output output;
    output.depth = input.position.z;
    output.surface = surface;
    return output;
}
)";

// The rays' shadow where the surface drew: a multiply of the scene by the traced shadow, so the map's,
// which the draw left out, is the rays' for those pixels. A water surface is mostly sky reflected in it:
// blocking the sun hardly changes that, so the surface barely dims - the shadow is the bed's, a dark patch
// through the water column, which the deferred lighting over the bed already carries.
inline constexpr char Multiply[] = R"(
Texture2D<float> Shadow : register(t0);
static const float ShadeFloor = 0.95f;

float4 main(float4 position : SV_Position) : SV_Target
{
    const float shadow = Shadow.Load(int3(int2(position.xy), 0));
    const float lit = lerp(ShadeFloor, 1.0f, shadow);
    return float4(lit, lit, lit, 0.0f);
}
)";


}
