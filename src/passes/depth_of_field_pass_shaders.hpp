#pragma once

#include "features/hdr/hdr_shaders.hpp"

// HLSL for the depth of field's focus edges (depth_of_field_pass.hpp), compiled at runtime with
// d3dcompiler_47.
namespace phantom_fx::depth_of_field_pass::shaders
{
// Each render pixel's own cover by the far layer, as DOF_ScatterCompositeFar works a half-size pixel's
// out: its blur radius from its depth (DOF_ScatterBakeFirst), then a ramp over the lower half of the
// sizes the blur takes. Drawn at DOF_ScatterBakeFirst, while the game's constants are bound.
inline constexpr char Coverage[] = R"(
cbuffer cPSScene : register(b2)
{
    float4 SceneRegisters[21];
    float4 ProjectionParam; // c21: z and w turn device depth into view depth
};
cbuffer cPSObject : register(b5)
{
    float4 ObjectRegisters[11];
    float4 CocLimits; // c11: z the largest far radius
    float4 CocSizes;  // c12: x and y the sizes the blur takes, z the radius's scale, w the focus distance
};
Texture2D<float> Depth : register(t1);

// x the game's cover; y the far radius in render pixels, the game's being in pixels of a 1920-wide image.
float2 main(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    Depth.GetDimensions(width, height);

    const float depth = Depth.Load(int3(position.xy, 0));
    const float viewZ = ProjectionParam.z / (depth - ProjectionParam.w);
    const float radius = min(CocSizes.z * (viewZ - CocSizes.w) / viewZ, CocLimits.z);
    const float cover = saturate((radius - CocSizes.x) * 2 / max(CocSizes.y - CocSizes.x, 1e-4));

    // The game's blur starts at its smallest size; from there the radius is the lens's own.
    const float start = saturate((radius - CocSizes.x) / max(CocSizes.x * 0.5, 1e-4));

    return float2(cover, max(radius, 0) * start * width / 1920);
}
)";

// The far blur's light at half the render size, in linear light. What is in focus stays out, so it cannot
// spread into the blur around it, and each pixel counts less the brighter it is (Karis's weight), so one
// noisy ray cannot turn into a disc. a = the radius in half-size texels; rgb is multiplied by it, which
// lets a bilinear tap beside an in-focus texel keep its colour: rgb / a.
inline constexpr char FarPrefilter[] = PHANTOM_FX_HDR_COMMON R"(
Texture2D<float4> Scene : register(t1);
Texture2D<float2> Blur : register(t2);

static const float SharpRadius = 0.5; // Render pixels

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 corner = int2(position.xy) * 2;

    float3 light = 0;
    float radius = 0, total = 0;
    for (int i = 0; i < 4; ++i)
    {
        const int3 texel = int3(corner + int2(i & 1, i >> 1), 0);
        const float own = Blur.Load(texel).y;
        if (own <= SharpRadius) continue;

        const float3 color = SrgbToLinear(Scene.Load(texel).rgb);
        const float weight = 1 / (1 + max(color.r, max(color.g, color.b)));
        light += color * weight;
        radius += own * weight;
        total += weight;
    }
    if (total == 0) return 0;

    const float halfSize = radius / total / 2;

    return float4(light / total * halfSize, halfSize);
}
)";

// A lens's disc, gathered: taps on a golden-angle spiral fill it evenly, and a tap counts if its own blur
// reaches this far (Jimenez 2014, "scatter as you gather"). The result is multiplied by how much of the
// disc had far light in it, which is what the fill and the composite weigh by.
inline constexpr char FarGather[] = R"(
Texture2D<float4> Prefiltered : register(t3);
SamplerState Linear : register(s11);

static const float LeastRadius = 2;     // So an in-focus texel still finds the light around it
static const float TapsPerArea = 3.2;   // Taps per squared radius, about one a texel
static const int MostTaps = 96;
static const float2 GoldenTurn = float2(-0.73736887, 0.67549029);

float4 main(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    Prefiltered.GetDimensions(width, height);
    const float2 texelSize = 1 / float2(width, height);
    const float2 uv = position.xy * texelSize;

    const float radius = max(Prefiltered.Load(int3(position.xy, 0)).a, LeastRadius);
    const int taps = min(int(TapsPerArea * radius * radius), MostTaps);

    float4 sum = 0;
    float2 direction = float2(1, 0);
    for (int i = 0; i < taps; ++i)
    {
        const float distance = radius * sqrt((i + 0.5) / taps);
        const float4 tap = Prefiltered.SampleLevel(Linear, uv + direction * distance * texelSize, 0);
        const float weight = saturate(tap.a - distance + 1);
        sum += float4(tap.rgb / max(tap.a, 1e-4), 1) * weight;
        direction = float2(direction.x * GoldenTurn.x - direction.y * GoldenTurn.y, direction.x * GoldenTurn.y + direction.y * GoldenTurn.x);
    }

    return sum / taps;
}
)";

// Evens out what the gather's taps left between them (Jimenez 2014's second pass): a ring as wide as the
// taps are apart, on the gather's multiplied light, so this is a plain average.
inline constexpr char FarFill[] = R"(
Texture2D<float4> Gathered : register(t3);
Texture2D<float4> Prefiltered : register(t4);
SamplerState Linear : register(s11);

static const float2 Ring[8] = {float2(0.92, 0.38), float2(0.38, 0.92), float2(-0.38, 0.92), float2(-0.92, 0.38),
                               float2(-0.92, -0.38), float2(-0.38, -0.92), float2(0.38, -0.92), float2(0.92, -0.38)};

float4 main(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    Gathered.GetDimensions(width, height);
    const float2 texelSize = 1 / float2(width, height);
    const float2 uv = position.xy * texelSize;

    const float spacing = Prefiltered.Load(int3(position.xy, 0)).a * 0.18;
    float4 sum = Gathered.Load(int3(position.xy, 0));
    if (spacing < 0.75) return sum;

    for (int i = 0; i < 8; ++i)
        sum += Gathered.SampleLevel(Linear, uv + Ring[i] * spacing * texelSize, 0);

    return sum / 9;
}
)";

// DOF_ScatterCompositeFinal, with the layers apart: the near layer over the far layer over the scene, in
// the sRGB-encoded scene's light. The half-size far layer gives the blur's colour alone; how much of it
// shows is the render pixel's own cover. Where a half-size pixel counted as blurred for one background
// pixel of its four, the in-focus pixels beside it stay sharp, and the background beside an in-focus
// half-size pixel takes all of the blur.
#define PHANTOM_FX_DOF_COMPOSITE PHANTOM_FX_HDR_COMMON R"(
Texture2D<float4> Near : register(t0);
Texture2D<float4> Scene : register(t1);
Texture2D<float2> Blur : register(t2);
Texture2D<float4> Far : register(t3);
SamplerState Linear : register(s11);

// Below this cover the far layer's colour is too little to stand for the whole pixel.
static const float LeastFarCover = 0.05;

float2 UvOf(float4 position)
{
    uint width, height;
    Scene.GetDimensions(width, height);

    return position.xy / float2(width, height);
}

float4 Composited(float4 position, float4 far, float cover)
{
    const float4 near = Near.Sample(Linear, UvOf(position));
    const float4 scene = Scene.Load(int3(position.xy, 0));

    const float3 farColor = far.rgb / max(far.a, 1e-4);
    const float3 behindNear = lerp(SrgbToLinear(scene.rgb), farColor, cover * saturate(far.a / LeastFarCover));

    return float4(LinearToSrgb(near.rgb + (1 - saturate(near.a)) * behindNear), scene.a);
}
)"

// With the game's far blur. Where the two covers agree this is the game's blend.
inline constexpr char Composite[] = PHANTOM_FX_DOF_COMPOSITE R"(
float4 main(float4 position : SV_Position) : SV_Target
{
    return Composited(position, Far.Sample(Linear, UvOf(position)), Blur.Load(int3(position.xy, 0)).x);
}
)";

// With the lens's far blur. Its half-size layer is stretched by a cubic B-spline in four bilinear taps
// (Sigg and Hadwiger 2005), as a bilinear stretch shows its texels. The cover is the pixel's own radius:
// sharp under half a render pixel, all blur over one and a half.
inline constexpr char LensComposite[] = PHANTOM_FX_DOF_COMPOSITE R"(
float4 Smooth(Texture2D<float4> layer, float2 uv)
{
    uint width, height;
    layer.GetDimensions(width, height);
    const float2 size = float2(width, height);

    const float2 position = uv * size - 0.5;
    const float2 base = floor(position);
    const float2 f = position - base;
    const float2 f2 = f * f;
    const float2 f3 = f2 * f;
    const float2 w0 = (1 - 3 * f + 3 * f2 - f3) / 6;
    const float2 w1 = (4 - 6 * f2 + 3 * f3) / 6;
    const float2 w2 = (1 + 3 * f + 3 * f2 - 3 * f3) / 6;
    const float2 w3 = f3 / 6;
    const float2 g0 = w0 + w1;
    const float2 g1 = w2 + w3;
    const float2 h0 = (base - 0.5 + w1 / g0) / size;
    const float2 h1 = (base + 1.5 + w3 / g1) / size;

    return g0.y * (g0.x * layer.SampleLevel(Linear, h0, 0) + g1.x * layer.SampleLevel(Linear, float2(h1.x, h0.y), 0))
         + g1.y * (g0.x * layer.SampleLevel(Linear, float2(h0.x, h1.y), 0) + g1.x * layer.SampleLevel(Linear, h1, 0));
}

float4 main(float4 position : SV_Position) : SV_Target
{
    return Composited(position, Smooth(Far, UvOf(position)), saturate(Blur.Load(int3(position.xy, 0)).y - 0.5));
}
)";
}
