#pragma once

// HLSL for the UI at the output size (ui_redirect.cpp), compiled at runtime with d3dcompiler_47.
namespace phantom_fx::ui_redirect::shaders
{
// A triangle over the whole target, with the texture coordinate across it.
inline constexpr char WholeTarget[] = R"(
void main(uint vertex : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((vertex << 1) & 2, vertex & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";

// The scene's depth as it would be without the upscaler's jitter, into a depth-stencil of the
// target's size. The scene was drawn jittered: what lies at a place on screen is in the depth that
// much further along, so it is read there, bilinearly. Nearest, an edge would land on one pixel or
// its neighbour as the jitter turns; interpolated, it crosses a plane where the scene has it. Hair
// is dithered into the depth, with holes a pixel wide that the image's upscaler fills but the depth
// keeps. Each of the four reads is closed first: it comes forward to a pair of opposite neighbours
// when both are nearer, which fills a hole or a gap a pixel wide and leaves an outline where it is.
// The nearest of 2x2 closed them too, but grew every outline by a render pixel, a halo of backdrop
// between a character and the emblem.
inline constexpr char StretchDepth[] = R"(
Texture2D<float> Depth : register(t0);

cbuffer Fill : register(b0)
{
    float2 Jitter;  // Render pixels, x right and y down
    float Nearer;   // 1 where nearer is more (reversed depth), -1 where it is less
};

float main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Depth
{
    uint width, height;
    Depth.GetDimensions(width, height);
    float2 at = uv * float2(width, height) + Jitter - 0.5;
    int2 base = int2(floor(at));
    float2 weight = at - base;
    int2 last = int2(width, height) - 1;

    // The four reads at d[1..2][1..2], with a pixel around them.
    float d[4][4];
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            d[y][x] = Nearer * Depth.Load(int3(clamp(base + int2(x - 1, y - 1), 0, last), 0));

    float closed[2][2];
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
        {
            int y = j + 1, x = i + 1;
            float pairs = max(max(min(d[y][x - 1], d[y][x + 1]), min(d[y - 1][x], d[y + 1][x])),
                max(min(d[y - 1][x - 1], d[y + 1][x + 1]), min(d[y - 1][x + 1], d[y + 1][x - 1])));
            closed[j][i] = max(d[y][x], pairs);
        }

    return Nearer * lerp(lerp(closed[0][0], closed[0][1], weight.x), lerp(closed[1][0], closed[1][1], weight.x), weight.y);
}
)";
}
