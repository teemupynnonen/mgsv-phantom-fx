// The colour encodings the scene target holds, and the deferred resolve's tonemap. The encodings are
// also used by the DLSS passes (features/upscaling/shaders/inputs.cs.hlsl).
StructuredBuffer<float4> Curve : register(t8);

// Linear colour from the sRGB-encoded scene, and back. Values above 1 keep their meaning.
float3 SrgbToLinear(float3 c)
{
    c = max(c, 0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 LinearToSrgb(float3 c)
{
    c = max(c, 0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055;
}

float Luminance(float3 linearBt709)
{
    return dot(linearBt709, float3(0.2126, 0.7152, 0.0722));
}

static const float HalfMax = 65504;

float3 Tonemap(float3 x)
{
    const float a = Curve[0].x, b = Curve[0].y, c = Curve[0].z;
    if (!(a > 0 && c > 0)) return x;

    return x <= b ? x : b + c - 1 / (a * (max(x, b) - b + c));
}

