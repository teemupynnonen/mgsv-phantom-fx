// The waves' spectrum at the frame's time, each tile's wave vectors from the centre of its N x N: four
// complex spectra in two slices, the inverse FFT's input. A pair of real fields goes into one complex
// field, a + ib, which comes back from the FFT as a in the real part and b in the imaginary.
#include "sea_common.hlsli"

RWStructuredBuffer<float4> g_Spectrum : register(u0); // Slices 2c and 2c + 1 for tile c

// PCG's hash (Jarzynski and Olano, "Hash Functions for GPU Rendering", 2020).
uint Hash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Two independent standard normal numbers (Box-Muller), the same for the same seed every frame.
float2 Gaussian(uint seed)
{
    float u1 = max((Hash(seed) & 0xFFFFFF) / 16777216.0, 1e-7);
    float u2 = (Hash(seed ^ 0x9E3779B9u) & 0xFFFFFF) / 16777216.0;
    return sqrt(-2 * log(u1)) * float2(cos(2 * Pi * u2), sin(2 * Pi * u2));
}

uint SeedOf(uint cascade, uint2 texel)
{
    return Hash((cascade * N + texel.y) * N + texel.x + 1);
}

// JONSWAP (Hasselmann et al., 1973) over the angular frequency.
float Jonswap(float omega)
{
    float peak = g_PeakFrequency;
    float sigma = omega <= peak ? 0.07 : 0.09;
    float r = exp(-(omega - peak) * (omega - peak) / (2 * sigma * sigma * peak * peak));
    return g_Alpha * Gravity * Gravity / pow(omega, 5) * exp(-1.25 * pow(peak / omega, 4)) * pow(g_PeakBoost, r);
}

// Donelan and Banner's spreading over the angle from the wind, -pi to pi.
float Spreading(float theta, float omega)
{
    float ratio = omega / g_PeakFrequency;
    float beta = ratio < 0.95 ? 2.61 * pow(ratio, 1.3) : ratio < 1.6 ? 2.28 * pow(ratio, -1.3) : pow(10, -0.4 + 0.8393 * exp(-0.567 * log(ratio * ratio)));
    float sech = 1 / cosh(beta * theta);
    return beta / (2 * tanh(beta * Pi)) * sech * sech;
}

// The spectrum over wave vectors, square metres times square metres: E(omega, theta) d omega / dk / k.
float SpectrumAt(float2 k)
{
    float length_ = length(k);
    float omega = sqrt(Gravity * length_);
    float2 wind = WindDirection();
    float theta = atan2(k.y, k.x) - atan2(wind.y, wind.x);
    theta -= 2 * Pi * round(theta / (2 * Pi));
    return Jonswap(omega) * Spreading(theta, omega) * (Gravity / (2 * omega)) / length_;
}

// p + i q
float2 Pack(float2 p, float2 q)
{
    return float2(p.x - q.y, p.y + q.x);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint cascade = id.z;
    float step = 2 * Pi / g_CascadeSizes[cascade];
    float2 k = (float2(id.xy) - N / 2) * step;
    float length_ = length(k);

    float4 first = 0, second = 0;
    if (length_ >= g_CascadeLow[cascade] && length_ < g_CascadeHigh[cascade])
    {
        // A wave's amplitude for the variance its share of the spectrum holds. The texel of -k uses the
        // seeds the other way round, so the field is Hermitian and its transform real.
        uint2 mirrored = (N - id.xy) % N;
        float2 here = Gaussian(SeedOf(cascade, id.xy)) * (0.5 * step * sqrt(SpectrumAt(k)));
        float2 opposite = Gaussian(SeedOf(cascade, mirrored)) * (0.5 * step * sqrt(SpectrumAt(-k)));
        float phase = sqrt(Gravity * length_) * g_Time;
        float2 turn = float2(cos(phase), sin(phase));
        // h0(k) e^(-i w t) + conj(h0(-k)) e^(i w t): the waves move along k.
        float2 h = Multiply(here, float2(turn.x, -turn.y)) + Multiply(float2(opposite.x, -opposite.y), turn);

        float2 ih = float2(-h.y, h.x);
        float2 direction = k / length_;
        float2 dx = -ih * direction.x, dz = -ih * direction.y; // The sideways displacement
        float2 hx = ih * k.x, hz = ih * k.y;                     // The slopes
        float2 dxx = h * (k.x * direction.x), dzz = h * (k.y * direction.y), dxz = h * (k.x * direction.y);
        first = float4(Pack(dx, h), Pack(dz, hx));
        second = float4(Pack(hz, dxx), Pack(dzz, dxz));
    }

    g_Spectrum[SpectrumIndex(id.xy, cascade * 2)] = first;
    g_Spectrum[SpectrumIndex(id.xy, cascade * 2 + 1)] = second;
}
