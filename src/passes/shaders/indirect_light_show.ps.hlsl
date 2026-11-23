#ifndef LIGHT
#define LIGHT 0
#endif
// Diagnostics: drawn in place of the deferred resolve, GTVBAO's occlusion (LIGHT 0) or SSILVB's light
// (LIGHT 1) alone. Every resolve vertex shader outputs the position first. The resolve writes through
// an sRGB view: the visibility is linearised first and stored as it is, rather than brightened by
// the encoding; SSILVB's light, which the last denoise pass writes without the game's diffuse light,
// is linear already.
#if LIGHT
Texture2D<float4> Light : register(t9);
#else
Texture2D<float> Visibility : register(t13);
#endif

float4 main(float4 position : SV_Position) : SV_Target
{
#if LIGHT
    float3 colour = Light[uint2(position.xy)].rgb;
#else
    float3 colour = pow(saturate(Visibility[uint2(position.xy)]), 2.2).xxx;
#endif
    // Alpha as the resolve writes it: a luminance code that exposure, bloom and depth of field read.
    // With 1 they took every pixel for a bright light and blurred the whole view.
    float luminance = dot(colour, float3(0.2125, 0.7154, 0.0721)) / 32;
    return float4(colour, luminance * rsqrt(max(luminance, 1.0 / 512)));
}
