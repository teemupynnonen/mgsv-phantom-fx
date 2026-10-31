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

float main(float4 position : SV_Position) : SV_Target
{
    const float depth = Depth.Load(int3(position.xy, 0));
    const float viewZ = ProjectionParam.z / (depth - ProjectionParam.w);
    const float radius = min(CocSizes.z * (viewZ - CocSizes.w) / viewZ, CocLimits.z);

    return saturate((radius - CocSizes.x) * 2 / max(CocSizes.y - CocSizes.x, 1e-4));
}
)";

// DOF_ScatterCompositeFinal, with the layers apart: the near layer over the far layer over the scene, in
// the sRGB-encoded scene's light. The half-size far layer gives the blur's colour alone; how much of it
// shows is the render pixel's own cover. Where the two covers agree this is the game's blend. Where a
// half-size pixel counted as blurred for one background pixel of its four, the in-focus pixels beside
// it stay sharp, and the background beside an in-focus half-size pixel takes all of the blur.
inline constexpr char Composite[] = PHANTOM_FX_HDR_COMMON R"(
Texture2D<float4> Near : register(t0);
Texture2D<float4> Scene : register(t1);
Texture2D<float> Coverage : register(t2);
Texture2D<float4> Far : register(t3);
SamplerState Linear : register(s11);

// Below this cover the far layer's colour is too little to stand for the whole pixel.
static const float LeastFarCover = 0.05;

float4 main(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    Scene.GetDimensions(width, height);
    const float2 uv = position.xy / float2(width, height);
    const float4 near = Near.Sample(Linear, uv);
    const float4 far = Far.Sample(Linear, uv);
    const float4 scene = Scene.Load(int3(position.xy, 0));

    const float3 farColor = far.rgb / max(far.a, 1e-4);
    const float cover = Coverage.Load(int3(position.xy, 0)) * saturate(far.a / LeastFarCover);
    const float3 behindNear = lerp(SrgbToLinear(scene.rgb), farColor, cover);

    return float4(LinearToSrgb(near.rgb + (1 - saturate(near.a)) * behindNear), scene.a);
}
)";
}
