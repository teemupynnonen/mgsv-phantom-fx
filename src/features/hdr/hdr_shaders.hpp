#pragma once

// HLSL for HDR output (hdr.hpp), compiled at runtime with d3dcompiler_47.
namespace phantom_fx::hdr::shaders
{
// The colour encodings the scene target holds, and the deferred resolve's tonemap. The encodings are
// also used by the DLSS passes (upscaler_shaders.hpp).
//
// TppTonemap: x up to b, then b + c - 1 / (a (x - b + c)), which approaches b + c. The 8-bit target
// clamped its result to 1, which it reaches at x = 4 with the usual curve. Curve[0] holds the
// resolve's (a, b, c), captured each frame; zeros, before the first capture, leave colours as they are.
#define PHANTOM_FX_HDR_COMMON R"(
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
)"

// At a deferred resolve: its cPSMaterial c0, TppTonemap's (a, b, c).
inline constexpr char Capture[] = R"(
cbuffer GameMaterial : register(b1)
{
    float4 GameMaterial[1];
};

RWStructuredBuffer<float4> OutCurve : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    OutCurve[0] = GameMaterial[0];
}
)";

// After a draw through an sRGB render target view of a float target, which stored linear colour:
// encodes it, as the 8-bit target's view did on writing.
inline constexpr char Encode[] = PHANTOM_FX_HDR_COMMON R"(
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
)";

// After a copy into a float target that is read through sRGB views: its decoded twin, which those
// views read instead. The reflections write 8-bit targets, which clip their light at 1, but they
// also add themselves to the scene they read here, whose light stays.
inline constexpr char Decode[] = PHANTOM_FX_HDR_COMMON R"(
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
)";

// In place of CopyReductionX4 on the scene: the scene at a quarter of its size, as bloom and the lens
// flares start from it, through TppTonemap as the game had it. Four bilinear taps, as the game's.
inline constexpr char Reduce[] = PHANTOM_FX_HDR_COMMON R"(
// CopyReductionX4's offset and scale of its texture coordinates are its c12.
cbuffer cPSObject : register(b5)
{
    float4x4 m_viewWorld;
    float4x4 m_world;
    float4 m_useWeightCount;
    float4 m_localParam[4];
};

Texture2D<float4> inImage : register(t0);
SamplerState g_samplerLinear_Clamp : register(s11);

float4 Tap(float2 uv, int2 offset)
{
    const float4 color = inImage.Sample(g_samplerLinear_Clamp, uv, offset);
    return float4(LinearToSrgb(saturate(Tonemap(SrgbToLinear(color.rgb)))), color.a);
}

float4 main(float4 position : SV_Position, float2 clip : TEXCOORD0) : SV_Target
{
    const float2 uv = (float2(clip.x, -clip.y) * 0.5 + 0.5) * m_localParam[3].zw + m_localParam[3].xy;
    return (Tap(uv, int2(-1, -1)) + Tap(uv, int2(1, -1)) + Tap(uv, int2(-1, 1)) + Tap(uv, int2(1, 1))) / 4;
}
)";

// In place of the game's grading pass (Tonemap_ps). The scene holds light, which goes through
// TppTonemap as the game had it, then the game's own grading: the bloom screened over it and the
// 16x16x16 colour LUT; in SDR that is the image. With HDR output (UPGRADE_TONEMAP) the light the
// tonemap took is then given back as RenoDX's UpgradeToneMap does (MIT, RenoDX-LICENSE.txt): added to
// the graded luminance, with the graded colour scaled to it, so highlights keep the grade's colour.
// Below the curve's knee nothing was taken, so the image is the game's. Alpha keeps the game's luma,
// which FXAA reads.
inline constexpr char Grade[] = PHANTOM_FX_HDR_COMMON R"(
cbuffer cPSSystem : register(b0)
{
    float4 m_param;
    float4 m_renderInfo;
    float4 m_renderBuffer;
    float4 m_dominantLightDir;
};

Texture2D<float4> inImage : register(t0);
Texture2D<float4> inColorLUT : register(t6);
Texture2D<float4> inBloom : register(t7);
SamplerState g_samplerPoint_Wrap : register(s8);
SamplerState g_samplerLinear_Clamp : register(s11);

float3 ColorCorrection(float3 color)
{
    const float3 uvw = saturate(color) * 0.9375;
    const float slice = floor(uvw.b * 16) * 0.0625;
    const float blend = (uvw.b - slice) * 16;
    const float2 uv = float2(uvw.r * 0.0625 + slice, uvw.g) + float2(1.0 / 512, 1.0 / 32);
    const float3 low = inColorLUT.Sample(g_samplerLinear_Clamp, uv).rgb;
    const float3 high = inColorLUT.Sample(g_samplerLinear_Clamp, uv + float2(0.0625, 0)).rgb;
    return low * (1 - blend) + high * blend;
}

float4 main(float2 clip : TEXCOORD0, float4 position : SV_Position) : SV_Target
{
    const float2 pixelSize = m_renderBuffer.zw;
    const float2 bloomUv = float2(clip.x, -clip.y) * 0.5 + 0.5 + pixelSize * 2;
    const float3 light = min(SrgbToLinear(inImage.Sample(g_samplerPoint_Wrap, position.xy * pixelSize).rgb), HalfMax);
    const float3 toned = saturate(Tonemap(light));
    const float3 scene = LinearToSrgb(toned);

    // The game screens the bloom over the scene with gamma 2 standing in for sRGB.
    const float3 squared = scene * scene;
    const float3 bloom = min(inBloom.Sample(g_samplerLinear_Clamp, bloomUv).rgb, 0.5);
    float3 color = saturate(squared + bloom - squared * bloom);
    color *= rsqrt(max(color, 1.0 / 512));
    color = ColorCorrection(color);
    const float luma = dot(color, float3(0.299, 0.587, 0.114));

#if !UPGRADE_TONEMAP
    return float4(color, luma);
#else
    const float3 graded = SrgbToLinear(color);
    const float taken = max(Luminance(light) - Luminance(toned), 0);
    const float gradedLuminance = Luminance(graded);
    const float scale = gradedLuminance > 0 ? (gradedLuminance + taken) / gradedLuminance : 0;
    return float4(LinearToSrgb(graded * scale), luma);
#endif
}
)";

// In place of GammaControl, which wrote the back buffer: the game's brightness setting as it applies
// it, then the display's encoding. HDR10 is PQ with BT.2020 primaries, the game's white at the paper
// white and the brightest the image can be at the display's peak; otherwise sRGB with highlights
// clipped, as the game showed it. The back buffer is R10G10B10A2 either way, so HDR output can turn
// on and off without a new swap chain.
inline constexpr char Output[] = PHANTOM_FX_HDR_COMMON R"(
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
)";
}
