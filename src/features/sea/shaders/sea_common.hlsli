// Every pass's constants (sea.cpp's Constants), and the tiles.
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

