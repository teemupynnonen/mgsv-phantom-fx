#pragma once

// The sea's shaders (sea.hpp). The waves are Tessendorf's ("Simulating Ocean Water", 2001): a spectrum of
// wave vectors, each wave moving at its deep-water speed, summed by an inverse FFT into heights,
// sideways displacement (his choppy waves) and slopes, in three tiles of different sizes. The spectrum is
// JONSWAP's with Donelan and Banner's spreading, as Horvath gives them ("Empirical directional wave
// spectra for computer graphics", 2015). The surface is a set of rings around the eye, finer near it
// (Losasso and Hoppe's geometry clipmaps, 2004), whose vertices morph to the next ring's at their edge as
// in Strugar's CDLOD (2009), so no crack opens between them.
namespace phantom_fx::sea::shaders
{
// Every pass's constants (sea.cpp's Constants), and the tiles.
constexpr char Common[] = R"(
#define CASCADES 3
#define N 256
#define LOG_N 8

cbuffer Sea : register(b9)
{
    float4 g_CascadeSizes; // xyz: each tile's size, in metres
    float4 g_CascadeLow;   // xyz: the wavenumber (radians a metre) each tile's waves start at
    float4 g_CascadeHigh;  // xyz: and the one they end before
    float g_Alpha;          // JONSWAP's energy scale, the peak's angular frequency and its peak boost for the wind
    float g_PeakFrequency;
    float g_PeakBoost;
    float g_Time;           // Seconds
    float g_FoamLeft;       // The share of the foam a frame leaves
    float g_Choppiness;     // The sideways displacement's scale
    float g_GridSpacing;    // The finest ring's, in metres
    uint g_GridCells;       // A ring's cells across
    float g_MicroRoughness; // The squared slope of the waves too short for the tiles
    float3 g_WaterAlbedo;   // Linear: the light the water sends back up as a share of what falls on it (GI_SEA_ALBEDO)
    float g_HasGameFoam;    // 1 when the game's foam textures are bound (the surface's t3 and t4)
    float g_FoamCoverage;   // How far short of folding foam shows: the squeeze at which the crests' foam starts
    float g_HeightScale;    // How tall the geometry stands: heights alone, the slopes (and so the normals) kept
};

// The game's own near sea draws' material constants (cVSMaterial), as they last ran; zero before.
cbuffer GameSea : register(b10)
{
    float4 g_GameSea[8]; // c0.z: the sea's level; c1.xy: where the wind blows to, in the world's xz
};

float SeaLevel()
{
    return g_GameSea[0].z;
}

// Before the game's: a south-easterly trade wind.
float2 WindDirection()
{
    float2 wind = g_GameSea[1].xy;
    return dot(wind, wind) > 1e-6 ? normalize(wind) : float2(-0.6, 0.8);
}

static const float Pi = 3.14159265;
static const float Gravity = 9.81;

// Two complex numbers' product.
float2 Multiply(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// The spectrum's texel in its buffer: N x N a slice, two slices a tile.
uint SpectrumIndex(uint2 texel, uint slice)
{
    return (slice * N + texel.y) * N + texel.x;
}
)";

// The waves' spectrum at the frame's time, each tile's wave vectors from the centre of its N x N: four
// complex spectra in two slices, the inverse FFT's input. A pair of real fields goes into one complex
// field, a + ib, which comes back from the FFT as a in the real part and b in the imaginary.
constexpr char Spectrum[] = R"(
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
)";

// One line of N complex pairs through the inverse FFT, in place: Stockham's radix-2 (Govindaraju et al.,
// "High Performance Discrete Fourier Transforms on Graphics Processors", 2008). COLUMNS takes the
// columns, otherwise the rows. A thread group a line and slice.
constexpr char Fft[] = R"(
RWStructuredBuffer<float4> g_Spectrum : register(u0);

groupshared float4 g_Line[2][N];

// The index'th of a row or column (`strip`) in a slice.
uint IndexOf(uint strip, uint index, uint slice)
{
#if COLUMNS
    return SpectrumIndex(uint2(strip, index), slice);
#else
    return SpectrumIndex(uint2(index, strip), slice);
#endif
}

[numthreads(N / 2, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    g_Line[0][thread] = g_Spectrum[IndexOf(group.x, thread, group.y)];
    g_Line[0][thread + N / 2] = g_Spectrum[IndexOf(group.x, thread + N / 2, group.y)];
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint stage = 0; stage < LOG_N; ++stage)
    {
        uint from = stage & 1, size = 1u << stage, within = thread % size;
        float angle = Pi * within / size;
        float2 twiddle = float2(cos(angle), sin(angle));
        float4 a = g_Line[from][thread];
        float4 b = g_Line[from][thread + N / 2];
        b = float4(Multiply(b.xy, twiddle), Multiply(b.zw, twiddle));
        uint to = (thread / size) * size * 2 + within;
        g_Line[1 - from][to] = a + b;
        g_Line[1 - from][to + size] = a - b;
        GroupMemoryBarrierWithGroupSync();
    }

    g_Spectrum[IndexOf(group.x, thread, group.y)] = g_Line[LOG_N & 1][thread];
    g_Spectrum[IndexOf(group.x, thread + N / 2, group.y)] = g_Line[LOG_N & 1][thread + N / 2];
}
)";

// The transformed fields into the maps the surface reads, each tile a slice: the displacement, the slopes
// with the sideways stretch, and the slopes' squares with the foam, whose levels of detail then give the
// slopes' spread over a pixel (LEAN mapping: Olano and Baker, 2010; for the sea, Bruneton et al., "Real-time
// Realistic Ocean Lighting using Seamless Transitions from Geometry to BRDF", 2010). Foam gathers where the
// surface folds (the Jacobian of the displacement falls low) and fades over seconds.
constexpr char Maps[] = R"(
RWStructuredBuffer<float4> g_Spectrum : register(u0);
RWTexture2DArray<float4> g_Displacement : register(u1); // Sideways x, height, sideways z
RWTexture2DArray<float4> g_Derivatives : register(u2);  // dh/dx, dh/dz, dDx/dx, dDz/dz
RWTexture2DArray<float4> g_Moments : register(u3);      // (dh/dx)^2, (dh/dz)^2, squeezed, foam
RWStructuredBuffer<float> g_Foam : register(u4);        // The foam kept from frame to frame, N x N a tile
RWStructuredBuffer<uint> g_Deepest : register(u5);      // Each tile's deepest trough below the level, metres (float bits)

groupshared uint g_GroupDeepest;

static const float FoamBelow = 0.55; // The Jacobian below which crests break
static const float FoamSharpness = 4;
static const float SqueezedBelow = 0.9; // The Jacobian below which the game's foam patterns start to show

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    // The transform sums e^(i 2 pi m n / N) from the spectrum's first texel; its wave vectors start at
    // -N / 2, which flips the sign of every other texel.
    float sign = ((id.x + id.y) & 1) ? -1 : 1;
    float4 first = g_Spectrum[SpectrumIndex(id.xy, id.z * 2)] * sign;
    float4 second = g_Spectrum[SpectrumIndex(id.xy, id.z * 2 + 1)] * sign;

    float dx = first.x * g_Choppiness, height = first.y, dz = first.z * g_Choppiness, hx = first.w;
    float hz = second.x, dxx = second.y * g_Choppiness, dzz = second.z * g_Choppiness, dxz = second.w * g_Choppiness;
    float jacobian = (1 + dxx) * (1 + dzz) - dxz * dxz;
    // The smallest tile's ripples do not break.
    uint kept = SpectrumIndex(id.xy, id.z);
    float foam = g_Foam[kept] * g_FoamLeft;
    // Where the surface is squeezed, short of folding: the crests' fronts, which the game's whitecap mask covers.
    // The setting reaches both the folds and the squeeze, so 0 leaves no foam at all, not just the squeeze's. Its
    // whole range counts: above 1 the squeeze reaches further out, while the folds are already all foam at 1.
    float squeezed = 0;
    float coverage = max(g_FoamCoverage, 0);
    if (id.z < CASCADES - 1 && coverage > 0)
    {
        foam = max(foam, saturate((FoamBelow - jacobian) * FoamSharpness) * min(coverage, 1));
        // Higher waves fold more and show their foam over more of the sea; the setting widens that reach.
        float squeezedBelow = FoamBelow + (SqueezedBelow - FoamBelow) * coverage;
        squeezed = saturate((squeezedBelow - jacobian) / (squeezedBelow - FoamBelow));
    }
    else
    {
        foam = 0;
    }
    g_Foam[kept] = foam;

    g_Displacement[id] = float4(dx, height, dz, 0);
    g_Derivatives[id] = float4(hx, hz, dxx, dzz);
    g_Moments[id] = float4(hx * hx, hz * hz, squeezed, foam);

    // The ray-traced scene's plane lies under the waves (sea::planeLevel): the tiles' troughs together are as deep as
    // the surface goes. Positive floats order as their bits do; a group's deepest first, then one atomic a group.
    if (index == 0) g_GroupDeepest = 0;
    GroupMemoryBarrierWithGroupSync();
    InterlockedMax(g_GroupDeepest, asuint(max(-height * g_HeightScale, 0)));
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) InterlockedMax(g_Deepest[id.z], g_GroupDeepest);
}
)";

// The surface, drawn into the G-buffer as the game's objects are: one draw of every ring, an instance a
// ring, with no vertex buffer. It reads the game's own camera (cVSScene, b2), jitter and all.
constexpr char Surface[] = R"(
cbuffer GameScene : register(b2)
{
    float4 g_Scene[30]; // c0-c3 projection x view, c8-c10 world to view (docs/DEVELOPMENT.md, "Constant buffers")
};

Texture2DArray<float4> g_Displacement : register(t0);
Texture2DArray<float4> g_Derivatives : register(t1);
Texture2DArray<float4> g_Moments : register(t2);
// The game's sea's own foam textures, as its near patches last bound them: its inFoamTexture (t3), a noise, and
// inFoamTexture2 (t5), three foam patterns from light to dense.
Texture2D<float4> g_GameFoamNoise : register(t3);
Texture2D<float4> g_GameFoamPattern : register(t4);
SamplerState g_Linear : register(s0);
SamplerState g_Anisotropic : register(s1);

// The game's near sea draws' pixel material constants (cPSMaterial), as they last ran: c0.xy the scroll, c1.z the
// foam's speed, c4.w its amount.
cbuffer GameSeaPixel : register(b11)
{
    float4 g_GameSeaPixel[8];
};

static const float EarthRadius = 6371000;
static const float3 FoamAlbedo = 0.7;
// The game's material table row: white specular, F0 0.027 (7/255), as near as it has to water's 0.02.
static const float WaterMaterial = 106.0 / 255;

struct Interpolants
{
    float4 position : SV_Position;
    float2 grid : TEXCOORD0;      // The world xz the waves move this point from
    float3 fromEye : TEXCOORD1;   // In the world
};

// The eye in the world, from the view: the scene constants' m_eyepos rises 35 m in fog.
float3 Eye()
{
    float3 t = float3(g_Scene[8].w, g_Scene[9].w, g_Scene[10].w);
    return -(t.x * g_Scene[8].xyz + t.y * g_Scene[9].xyz + t.z * g_Scene[10].xyz);
}

#if VERTEX
Interpolants main(uint vertex : SV_VertexID, uint ring : SV_InstanceID)
{
    Interpolants output = (Interpolants)0;
    float half_ = g_GridCells * 0.5;
    float2 grid = float2(vertex % (g_GridCells + 1), vertex / (g_GridCells + 1)) - half_;
    float spacing = g_GridSpacing * exp2(ring);
    float3 eye = Eye();
    // Each ring on its own grid, which moves in steps of two cells, so the next ring's vertices are every
    // other one of its.
    float2 centre = floor(eye.xz / (2 * spacing)) * (2 * spacing);
    float2 xz = centre + grid * spacing;

    // Inside the finer ring: the vertex is NaN, and no triangle that has it is drawn.
    if (ring > 0)
    {
        float finer = spacing * 0.5;
        float2 fromFiner = abs(xz - floor(eye.xz / (2 * finer)) * (2 * finer));
        if (max(fromFiner.x, fromFiner.y) < half_ * finer - 0.25 * spacing)
        {
            output.position = asfloat(0x7FC00000);
            return output;
        }
    }

    // Towards its edge a ring's odd vertices slide onto their even neighbours, the next ring's grid.
    float edge = max(abs(grid.x), abs(grid.y)) / half_;
    float morph = saturate((edge - 0.7) / 0.25);
    xz -= frac(grid * 0.5) * 2 * spacing * morph;

    // The level of detail of the cells the vertex stands for: the same on both sides of a ring's edge.
    float covered = spacing * (1 + morph);
    float3 displacement = 0;
    [unroll] for (uint cascade = 0; cascade < CASCADES; ++cascade)
    {
        float size = g_CascadeSizes[cascade];
        float level = max(log2(covered * N / size), 0);
        displacement += g_Displacement.SampleLevel(g_Linear, float3(xz / size, cascade), level).xyz;
    }

    // The height slider moves the geometry alone: the normals stay the waves' own, so raising the sea does not
    // sharpen every crest's shading with it.
    displacement.y *= g_HeightScale;

    // The Earth's curve, so the sea meets the sky at the horizon.
    float2 away = xz - eye.xz;
    float drop = dot(away, away) / (2 * EarthRadius);
    float3 world = float3(xz.x + displacement.x, SeaLevel() + displacement.y - drop, xz.y + displacement.z);
    float4 clip = float4(dot(g_Scene[0], float4(world, 1)), dot(g_Scene[1], float4(world, 1)), dot(g_Scene[2], float4(world, 1)),
        dot(g_Scene[3], float4(world, 1)));
    // Past the game's far plane, at its depth (reversed Z: 0 is the far plane), so the sea reaches the horizon.
    clip.z = max(clip.z, 1e-7 * clip.w);

    output.position = clip;
    output.grid = xz;
    output.fromEye = world - eye;
    return output;
}
#else
struct Targets
{
    float4 albedo : SV_Target0;   // sRGB
    float4 normal : SV_Target1;   // In view space, as EncodeViewSpaceNormal packs it
    float4 material : SV_Target2; // Roughness, specular intensity, material, translucency
    float depth : SV_Target3;     // The sea's own depth, which the G-buffer keeps where nothing nearer is drawn
};

float3 SrgbOf(float3 linear_)
{
    return linear_ <= 0.0031308 ? linear_ * 12.92 : 1.055 * pow(linear_, 1 / 2.4) - 0.055;
}

// The game's foam (Primitive_TppFourierOcean's NCalculate) over where the waves fold: its three patterns, the
// lighter where little folds and the denser where much does, broken up by its noise and thinned with distance.
// The game weighs them by its own whitecap mask; here it is where the surface is squeezed or folds (`folds`, 0 to 1).
float GameFoam(float2 xz, float folds, float distance)
{
    float2 uv = xz / max(g_GameSea[1].w, 1);
    float2 scroll = g_GameSeaPixel[0].xy;
    float speed = g_GameSeaPixel[1].z, amount = g_GameSeaPixel[4].w;
    float3 noise = g_GameFoamNoise.Sample(g_Anisotropic, uv * 0.2 + scroll).xyz;
    float3 pattern = g_GameFoamPattern.Sample(g_Anisotropic, uv + scroll * speed * 2).xyz;

    float strength = 1 + 10 * saturate(1 - amount);
    float far = saturate((distance - 100) / 1500);
    amount = lerp(1, noise.y, saturate(amount + far + 0.1));
    float folds2 = folds * folds;
    float near = (pattern.z * folds2 * folds2 + pattern.y * folds2 + pattern.x * folds) * noise.y * amount;
    near = near < 0.005 ? pow(max(near, 0), 1.5) : near;
    float distant = amount * noise.x * pattern.y * folds * 2 * (1 - far);
    return saturate(strength * lerp(near, distant, saturate((distance - 100) / 100)) * 0.5);
}

Targets main(Interpolants input)
{
    float2 slope = 0, stretch = 0, spread = 0;
    float foam = 0, squeezed = 0;
    [unroll] for (uint cascade = 0; cascade < CASCADES; ++cascade)
    {
        float3 uv = float3(input.grid / g_CascadeSizes[cascade], cascade);
        float4 derivatives = g_Derivatives.Sample(g_Anisotropic, uv);
        float4 moments = g_Moments.Sample(g_Anisotropic, uv);
        slope += derivatives.xy;
        stretch += derivatives.zw;
        spread += max(moments.xy - derivatives.xy * derivatives.xy, 0);
        foam += moments.w;
        squeezed += moments.z;
    }

    // The slope over the displaced surface: the sideways displacement stretches or squeezes it.
    slope /= max(1 + stretch, 0.25);
    float3 normal = normalize(float3(-slope.x, 1, -slope.y));
    // A normal facing away from the eye reflects into the sea: tilted back towards it.
    float3 toEye = -normalize(input.fromEye);
    float facing = dot(normal, toEye);
    if (facing < 0.05) normal = normalize(normal + (0.05 - facing) * toEye);

    // The slopes a pixel spreads over, with the ripples too short for the tiles: the roughness, as the
    // game keeps it (GGX's alpha squared, Beckmann's slope variance for both axes).
    float roughness = sqrt(sqrt(spread.x + spread.y + g_MicroRoughness));
    foam = saturate(foam);
    if (g_HasGameFoam > 0) foam = GameFoam(input.grid, saturate(foam + squeezed), length(input.fromEye));

    float3 viewNormal = float3(dot(g_Scene[8].xyz, normal), dot(g_Scene[9].xyz, normal), dot(g_Scene[10].xyz, normal));
    Targets output;
    output.albedo = float4(SrgbOf(lerp(g_WaterAlbedo, FoamAlbedo, foam)), 1);
    output.normal = float4(viewNormal.xy * 0.5 + 0.5, sqrt(saturate(viewNormal.z * 0.5 + 0.5)), 0);
    output.material = float4(lerp(roughness, 0.7, foam), lerp(1, 0.4, foam), WaterMaterial, 0);
    output.depth = input.position.z;
    return output;
}
#endif
)";
}
