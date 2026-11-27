// Runs once per frame at render resolution and writes what DLSS or FSR reads.
//
// Motion: the camera part is reprojected from depth with this and the previous frame's unjittered
// matrices, in float, so it is exact for everything that stands still. The engine also draws its
// dynamic meshes (characters, the horse, trees in the wind) into a velocity target, every frame
// whatever the game's motion blur setting (motion_blur.hpp): R = 1 where one drew, B and A the
// screen motion from the previous frame to this one in pixels / 64 (y down), stored as 0.5 v + 0.5
// in 8 bits. Each stored value covers a cell about 0.5 pixel wide, and the motion includes this
// frame's jitter, because the engine projects the current position with the jittered matrices and
// the previous one without. The object motion is the cell, less the jitter; the camera motion
// clamped into it keeps full precision where a character does not move.
//
// With DLSS Ray Reconstruction, the depth of glass that dominates a pixel takes the place of the scene's, and its motion
// with it: most glass draws no depth, so its cracks and dirt would move as what is behind them (forward_surfaces::guideReconstruction).
//
// With DLSS Ray Reconstruction on a frame the game's depth of field drew, also its depth of field guide:
// how much the depth of field changed each pixel's light, luminance(after - before) by NVIDIA's luminance
// (DLSS-RR Integration Guide, "Depth of Field Guide"). Ray Reconstruction runs after the depth of field,
// which blurs the noisy light; the guide tells it where.
#include "../../../shaders/hdr_common.hlsli"

cbuffer InputParameters : register(b0)
{
    float4 PreviousFromCurrent[4]; // Previous clip position from this frame's unjittered NDC and depth
    float2 JitterNdc;
    uint2 RenderSize;
    uint ObjectMotion; // t2 holds the engine's velocity
    uint Linear;       // The upscaler gets linear colour: FSR, or DLSS in its HDR mode
    uint DofGuide;     // t3 holds the scene before the depth of field: write u3
    uint Padding;
};
Texture2D<float4> SceneColor : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float4> Velocity : register(t2);
Texture2D<float4> SceneBeforeDof : register(t3);
Texture2D<float> GlassDepth : register(t4);
RWTexture2D<float4> OutColor : register(u0);
RWTexture2D<float> OutDepth : register(u1);
RWTexture2D<float2> OutMotion : register(u2);
RWTexture2D<float> OutDofGuide : register(u3);

static const float HalfCell = 64.0 / 255.0; // Half the engine's velocity step, in pixels

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= RenderSize)) return;

    float2 toPixels = float2(0.5, -0.5) * RenderSize;
    float2 ndc = (pixel + 0.5) / toPixels + float2(-1, 1) - JitterNdc;
    const float glassDepth = GlassDepth[pixel];
    const bool isGlass = glassDepth > 0;
    float depth = isGlass ? glassDepth : SceneDepth[pixel];
    float4 position = float4(ndc, depth, 1);
    float4 previous = float4(dot(PreviousFromCurrent[0], position), dot(PreviousFromCurrent[1], position),
        dot(PreviousFromCurrent[2], position), dot(PreviousFromCurrent[3], position));
    // Pixels from here to the previous position, which DLSS calls the motion vector.
    float2 motion = previous.w > 1e-6 ? (previous.xy / previous.w - ndc) * toPixels : 0;

    // The engine clamps its velocity to 64 pixels; beyond that only the camera motion is known.
    float4 velocity = Velocity[pixel];
    float2 stored = (velocity.ba * 2 - 1) * 64;
    if (!isGlass && ObjectMotion && velocity.r > 0.5 && length(stored) < 63)
    {
        float2 moved = stored - JitterNdc * toPixels;
        motion = clamp(motion, -moved - HalfCell, -moved + HalfCell);
    }

    const float4 color = SceneColor[pixel];
    OutColor[pixel] = Linear ? float4(min(SrgbToLinear(color.rgb), HalfMax), color.a) : color;
    OutDepth[pixel] = depth;
    OutMotion[pixel] = motion;

    if (DofGuide)
    {
        const float3 change = SrgbToLinear(color.rgb) - SrgbToLinear(SceneBeforeDof[pixel].rgb);
        OutDofGuide[pixel] = clamp(dot(change, float3(0.25, 0.5, 0.25)), -HalfMax, HalfMax);
    }
}
