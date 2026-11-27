// The composite shared by the game-far-blur and lens-far-blur versions.
#include "dof_common.hlsli"

Texture2D<float4> Near : register(t0);
Texture2D<float4> Scene : register(t1);
Texture2D<float2> Coverage : register(t2);
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

