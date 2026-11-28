// In place of GammaControl, which wrote the back buffer: the game's brightness setting as it applies
// it, then the display's encoding. HDR10 is PQ with BT.2020 primaries, the game's white at the paper
// white and the brightest the image can be at the display's peak; otherwise sRGB with highlights
// clipped, as the game showed it. The back buffer is R10G10B10A2 either way, so HDR output can turn
// on and off without a new swap chain.
#include "../../../shaders/hdr_common.hlsli"

cbuffer cPSSystem : register(b0)
{
    float4 m_param;
    float4 m_renderInfo;
    float4 m_renderBuffer;
    float4 m_dominantLightDir;
};

// GammaControl's offset, scale and gamma are its c9 to c11.
cbuffer cPSObject : register(b5)
{
    float4x4 m_viewWorld;
    float4x4 m_world;
    float4 m_useWeightCount;
    float4 m_localParam[4];
};

cbuffer OutputParameters : register(b2)
{
    float PaperWhite; // Nits
    float Peak;       // Nits
    uint Encoding;    // EncodingSrgb or EncodingPq
    float Padding;
};

// As hdr.cpp's Encoding: sRGB; HDR10.
static const uint EncodingSrgb = 0u;
static const uint EncodingPq = 1u;

Texture2D<float4> inImage : register(t0);
SamplerState g_samplerLinear_Clamp : register(s11);

static const float3x3 Bt709To2020 = {
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950,
};

float3 Brightness(float3 color)
{
    return max(pow(abs(color), m_localParam[2].rgb) * m_localParam[1].rgb + m_localParam[0].rgb, 0);
}

// SDR games were made on displays with gamma 2.2, which shows near black darker than the sRGB curve
// the game encodes with. As Luma and RenoDX do by default, the 0 to 1 range decodes with gamma 2.2;
// above 1 are only HDR's highlights, which the grading encoded with the sRGB curve.
float3 DisplayLinear(float3 encoded)
{
    encoded = max(encoded, 0);
    return encoded <= 1 ? pow(encoded, 2.2) : SrgbToLinear(encoded);
}

// The light that lands on the display's peak, in units of the game's white: RenoDX's default.
static const float WhiteClip = 100;

// RenoDX's Neutwo (MIT, RenoDX-LICENSE.txt): p x / sqrt(x^2 + p^2), with `clip` mapped exactly to the
// peak `p`. Nearly unchanged well below the peak, smooth all the way up. On the brightest channel, with
// the colour scaled along, so highlights keep their hue.
float3 RollOff(float3 color, float peak, float clip)
{
    const float brightest = max(color.r, max(color.g, color.b));
    if (clip <= peak || brightest <= 0) return color;

    const float rolled = clip * peak * brightest * rsqrt(brightest * brightest * (clip * clip - peak * peak) + clip * clip * peak * peak);
    return color * (min(rolled, peak) / brightest);
}

// SMPTE ST 2084, from nits / 10000.
float3 PerceptualQuantizer(float3 y)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 p = pow(saturate(y), m1);
    return pow((c1 + c2 * p) / (1 + c3 * p), m2);
}

// PCG (Jarzynski and Olano, "Hash Functions for GPU Rendering", 2020).
uint Pcg(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// The back buffer rounds a smooth gradient to steps a level apart, which at 8 bits showed as bands in
// fog and sky. Up to half a level either way, white noise by the pixel, turns each step's edge into
// grain too fine to see; at 10 bits it is a quarter of the 8-bit one, kept for displays that show 8.
// The game hid its own 8-bit steps with an 8x8 dither before the upscaler, which the upscaler turned
// into a grid; here it comes last, at the output size.
float3 Dithered(float3 encoded, uint2 pixel)
{
    const float noise = Pcg(pixel.x + Pcg(pixel.y)) * (1.0 / 4294967296.0);
    return saturate(encoded + (noise - 0.5) / 1023);
}

float4 main(float4 position : SV_Position, float2 texcoord : TEXCOORD0) : SV_Target
{
    const float2 uv = texcoord * m_renderInfo.xy * m_renderBuffer.zw;
    const float3 encoded = inImage.Sample(g_samplerLinear_Clamp, uv).rgb;
    const float3 sdr = LinearToSrgb(saturate(Brightness(SrgbToLinear(encoded))));
    if (Encoding == EncodingSrgb) return float4(Dithered(sdr, uint2(position.xy)), 1);

    const float3 color = max(mul(Bt709To2020, Brightness(DisplayLinear(encoded))), 0);
    return float4(PerceptualQuantizer(RollOff(color, Peak / PaperWhite, WhiteClip) * PaperWhite / 10000), 1);
}
