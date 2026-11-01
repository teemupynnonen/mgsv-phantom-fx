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

// The caustics a river puts on the bed under it: the same water geometry, drawn before the water blends over the
// scene, times the scene by the light its waves focus. The water then covers it, so the webs are read through the
// water, on the bed, not painted on the surface.
inline constexpr char Caustics[] = R"(
cbuffer Parameters : register(b0)
{
    row_major float4x4 viewProjectionInverse; // This frame's, the one the water is drawn with
    float4 eye;
    float4 dimensions;                        // xy: the render size; z: the frame's index; w: the strength
    float4 nearFar;                           // xy: the camera's near and far
};
Texture2D<float> BedDepth : register(t0);     // The bed's depth, as the kick copied it: the water column above it
Texture2D<float> CausticsMap : register(t1);  // The art's caustic web, tiling
SamplerState CausticsSampler : register(s0);  // Linear, wrap

// How many metres one tile of the map spans.
static const float TileMetres = 2.5f;

// The game's reversed-Z: 1 at the near plane, 0 at the far.
float ToLinear(float depth, float2 nearFar)
{
    return (nearFar.x * nearFar.y) / (nearFar.x + depth * (nearFar.y - nearFar.x));
}

// Two layers of the map, scrolled apart and at different scales, so the tile's repetition does not read: the product
// keeps the crosses where both carry a web. `lod` blurs it as the water deepens and the surface's focus spreads.
float CausticWeb(float2 world, float time, float lod)
{
    const float2 first = world * (1.0f / TileMetres) + time * float2(0.015f, 0.007f);
    const float2 second = world * (1.0f / 4.0f) - time * float2(0.011f, 0.014f);
    const float a = CausticsMap.SampleLevel(CausticsSampler, first, lod).x;
    const float b = CausticsMap.SampleLevel(CausticsSampler, second, lod + 0.5f).x;

    return a * b * 2.5f;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const float2 uv = position.xy / dimensions.xy;
    const float2 ndc = float2(2 * uv.x - 1, 1 - 2 * uv.y);

    // The web is on the bed, not the surface: the bed's own world position anchors it, so it stays with the
    // bottom when the water moves over it.
    const float bed = BedDepth.Load(int3(int2(position.xy), 0));
    const float4 bedClip = mul(viewProjectionInverse, float4(ndc, bed, 1));
    const float3 bedWorld = bedClip.xyz / bedClip.w;

    // The water column under this fragment: the bed is farther than the surface, both reversed-Z.
    const float under = max(ToLinear(bed, nearFar.xy) - ToLinear(position.z, nearFar.xy), 0.0f);

    // Nothing focuses where the water meets the bed, the shore or anything standing in it: the column goes to nothing,
    // so the contact ramps the webs in over the first few centimetres instead of cutting at the geometry's edge.
    // They are strongest just under the surface, and deep water spreads what the waves focus until it is gone.
    const float contact = smoothstep(0.0f, 0.25f, under);
    const float fade = exp(-under * (1.0f / 5.0f));
    const float lod = lerp(0.0f, 3.5f, saturate(under * (1.0f / 8.0f)));
    const float strength = dimensions.w * contact * fade;
    const float time = dimensions.z * (1.0f / 60.0f);
    const float shade = 1.0f + strength * CausticWeb(bedWorld.xz, time, lod);

    return float4(shade, shade, shade, 0.0f);
}
)";

}
