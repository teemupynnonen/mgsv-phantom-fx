// After a draw through an sRGB render target view of a float target, which stored linear colour:
// encodes it, as the 8-bit target's view did on writing.
#include "../../../shaders/hdr_common.hlsli"

RWTexture2D<float4> Scene : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height;
    Scene.GetDimensions(width, height);
    if (any(pixel >= uint2(width, height))) return;

    const float4 color = Scene[pixel];
    Scene[pixel] = float4(LinearToSrgb(color.rgb), color.a);
}
