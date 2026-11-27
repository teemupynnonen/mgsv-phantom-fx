// After a copy into a float target that is read through sRGB views: its decoded twin, which those
// views read instead. The reflections write 8-bit targets, which clip their light at 1, but they
// also add themselves to the scene they read here, whose light stays.
#include "../../../shaders/hdr_common.hlsli"

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Twin : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height;
    Twin.GetDimensions(width, height);
    if (any(pixel >= uint2(width, height))) return;

    const float4 color = Source[pixel];
    Twin[pixel] = float4(min(SrgbToLinear(color.rgb), HalfMax), color.a);
}
