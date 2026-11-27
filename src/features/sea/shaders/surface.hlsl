#ifndef VERTEX
#define VERTEX 0
#endif
// The surface, drawn into the G-buffer as the game's objects are: one draw of every ring, an instance a
// ring, with no vertex buffer. It reads the game's own camera (cVSScene, b2), jitter and all.
#include "sea_common.hlsli"

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
