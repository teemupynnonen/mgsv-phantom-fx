// The clouds' shared density (clouds.hlsl draws them, cloud_shadow.hlsl shadows the ground with them): the game's cloud
// constants, the add-on's 3D noise, and the one function that says how dense the layer is at a point of the world.
cbuffer cPSSystem : register(b0)
{
    float4 m_param, m_renderInfo, m_renderBuffer, m_dominantLightDir;
};

cbuffer cPSScene : register(b2)
{
    float4 m_scene[26]; // [20].xyz the eye, [23].z the exposure, [25].y the fog's normalising factor
};

cbuffer cPSMaterial : register(b4)
{
    // The game's cloud material: [0].xyz towards the sun, [0].w the phase's g; [1].xy the first offset,
    // [1].z the density's scale, [1].w the coverage; [2].xyz the ambient, [2].w the sun's scale;
    // [3].x the density's exponent, [3].y and z the cross sections, [3].w the sampling interval;
    // [5].x the depth, [5].zw the second offset; [6].x the density's base.
    float4 m_materials[8];
};

cbuffer cPSObject : register(b5)
{
    float4 m_object[13]; // [10].xyz the sun's light
};

// The menu's quality and tweakables (clouds.cpp, SettingsSlot). Steps the march's budget; the rest scale the model.
cbuffer cCloudSettings : register(b7)
{
    float g_cloudSteps, g_cloudCoverage, g_cloudSize, g_cloudWind, g_cloudShadow;
    float g_cloudOffsetX, g_cloudOffsetZ, g_cloudFog; // g_cloudFog: 1 the game's fog over the clouds, 0 none
    float g_cloudFrame; // counts the draws (clouds.cpp): the march's dither turns with it
    float g_cloudWeather; // the weather's cloud cover (clouds.cpp, CoverOf), steady through the night
};

// The add-on's own trilinear, wrapping sampler over every mip (clouds.cpp), bound for the draw.
SamplerState NoiseSampler : register(s12);
// The add-on's 64-cube of gradient-noise fbm and its mips (clouds.cpp), bound over the game's texture at t14 for the draw.
Texture3D<float> CloudNoise : register(t14);

// lod: log2 of how many of the cube's voxels one sample stands for. A far sample reads the mean of what its pixel and its step
// cover, not one speck of it: with the full-detail cube there, neighbouring pixels caught different specks and the deck grained.
float Noise3D(float3 p, float lod)
{
    return CloudNoise.SampleLevel(NoiseSampler, p, max(lod, 0)).x;
}

// The cube, turned about the vertical and scaled. The cube repeats every unit, and seen across a 100 km deck that repeat lines the
// clouds up in rows; two lookups whose lattices are turned and scaled apart never repeat together.
float Turned(float3 p, float angle, float scale, float offset, float lod)
{
    float c = cos(angle), s = sin(angle);
    return Noise3D(float3(c * p.x - s * p.z, p.y, s * p.x + c * p.z) * scale + offset, lod + log2(scale));
}

// How much faster than the true ground speed the clouds travel; 1 is the real wind.
static const float WindSpeed = 1;
// World heights, not the eye's: a mountain or an aircraft can climb into the layer.
static const float LayerBase = 900, LayerTop = 6000;
// Extinction per metre at density 1, the same for the view and the sun: one cloud, one opacity.
static const float Extinction = 0.012;

// The ground's cloud shadow (cloud_shadow.hlsl): a square of this many metres around the eye, in texels, indexed by where a
// point's ray to the sun crosses the layer's base.
static const float ShadowExtent = 30000, ShadowSize = 1024;

// The clouds' travelled distance, in the noise's units: it comes from the settings' own integral (clouds.cpp), so the wind
// slider changes the pace and leaves the clouds where they are.
// The distance the clouds have travelled, in metres (clouds.cpp, cloud-wind): the game's own cloud offset, a change at a time, so
// they move only with game time and its wind, never while the game is paused.
StructuredBuffer<float4> CloudWindState : register(t17);
float2 CloudWindXZ() { return CloudWindState[0].xy * 0.00005; }
float3 CloudWind() { float2 travelled = CloudWindState[0].xy; return float3(travelled.x, 0, travelled.y) * 0.00022; }
// The weather's coverage, steepened: fair weather (about 0.6) gives scattered clouds, an overcast sky (1) the full bank.
float CloudBaseCover(float domeAlpha)
{
    // The weather's, not the game's cloud material's: that fades to 0 at dusk and emptied the sky with it.
    float weather = pow(saturate(domeAlpha * g_cloudWeather), 1.5);
    // Below 100% the menu thins the weather's clouds; above, it adds cloud towards an overcast, so a clear day can still have a
    // full sky (a multiplier on a clear day's small coverage could not).
    float more = saturate(g_cloudCoverage - 1);
    return saturate(weather * min(g_cloudCoverage, 1) + more * (1 - weather) * 0.6);
}

// The density at one point of the world. Everything (the weather's coverage, the column's top, the shape) is looked
// up HERE, at this point, so the outline is a 3D shape and not a picture on the layer's base plane, and the march
// towards the sun sees where a neighbouring cloud begins and ends.
// footprint: how many metres one sample stands for, across its pixel and along its step; 0 reads full detail.
// coarse: the march towards the sun, which sees the cloud's bulk and not its rim; the erosion's two lookups are left at their mean.
float CloudDensity(float3 where, float2 windXZ, float3 wind, float baseCover, float footprint, bool coarse = false)
{
    float height = saturate((where.y - LayerBase) / (LayerTop - LayerBase));
    // Larger clouds from the same volume: a smaller frequency, so a cell spans more of the sky. Anchored on the world, so a cloud
    // stays where it is as the camera moves.
    float frequency = 1.0 / clamp(g_cloudSize, 0.25, 4.0);
    float3 g = float3(where.x * 0.00005 * frequency + windXZ.x, 0.5, where.z * 0.00005 * frequency + windXZ.y);
    // Two lookups summed have a narrower spread than one: 0.7 (about 1/sqrt 2) gives it back, so the thresholds below still hold.
    float group = 0.5 + (Noise3D(g, 0) + Turned(float3(g.x, 0.31, g.z), 1.21, 0.73, 0.17, 0) - 1) * 0.7;
    // The region lifts the cloud or sinks it, never empties it: the coverage itself is the share of the sky that has cloud.
    float cover = baseCover;
    if (cover <= 0) return 0;

    // Larger cells than before: one cloud is kilometres across, as a cumulus is.
    float3 p = where * (0.00022 * frequency) + wind;
    p.y *= 1.6;
    // The mip the footprint covers at the cube's own scale; each finer lookup adds its scale's log2 (Turned).
    float lod = log2(max(footprint * 64 * 0.00022 * frequency, 1e-6));
    float broad = 0.5 + (Noise3D(p, lod) + Turned(p, 0.83, 0.87, 0.29, lod) - 1) * 0.7;
    // Clear air, known before the finer lookup: the shape is at most broad * 0.65 + 0.35, and the mark it must pass is never below the
    // one at full cover. Most samples of a fair sky stop here.
    float lowestMark = lerp(0.72, 0.28, cover) + (0.5 - group) * 0.6 + (1 - saturate(cover * 4)) * 0.3;
    if (broad * 0.65 + 0.35 <= lowestMark) return 0;
    float base = broad * 0.65 + Turned(p, 2.07, 2.3, 0.37, lod) * 0.35;
    // One base for the whole layer reads as a shelf across the sky and one top as a lid: both take their height from the column's own
    // shape and its group, over a third of the layer or more, so a far deck ends at many heights instead of two planes.
    float baseShift = saturate(0.02 + 0.35 * (1 - group) + 0.45 * (base - 0.5));
    // Short of the layer's ceiling: a top that clamps to it puts every high cloud on one plane, which is exactly the flat lid this
    // is here to break.
    float columnTop = max(saturate(0.22 + 0.5 * base + 0.15 * (group - 0.5)), baseShift + 0.15);
    // A cloud is tallest at its core and low at its rim, like a cauliflower. A flat slab seen from below squashes into rows parallel to
    // the horizon; towers with depth break them up.
    // The group's clumps lift the mark by up to 0.18 on their own: below a quarter's cover the mark rises out of reach instead, so the
    // slider still clears the sky at its low end.
    float sparse = (1 - saturate(cover * 4)) * 0.3;
    float core = saturate((base - (lerp(0.72, 0.28, cover) + (0.5 - group) * 0.6 + sparse)) / 0.3);
    float lowTop = baseShift + 0.15;
    columnTop = lerp(lowTop, max(columnTop, lowTop), sqrt(core)) * lerp(1.0, 1.35, core);
    columnTop = min(columnTop, 1);
    float profile = smoothstep(baseShift, baseShift + 0.1, height) * (1 - smoothstep(columnTop * 0.7, columnTop, height));
    if (profile <= 0) return 0;
    // The coverage is the share of the sky that has cloud: the shape's own 10th and 90th are the ends the fill walks between, so
    // 100% is not a case apart from the rest, and the group lifts or sinks the mark. A cumulus narrows as it rises, so the fill
    // tightens with height and the top is a dome over a broad base.
    float fill = cover * lerp(1.0, 0.55, saturate(height / max(columnTop, 0.05)));
    float threshold = lerp(0.72, 0.28, fill) + (0.5 - group) * 0.6 + sparse;
    float mass = saturate((base - threshold) / 0.12) * profile;
    if (mass <= 0) return 0;
    // Erosion only at the rim: ragged wisps low down, rounded billows higher up. Two octaves, the second three times finer, so a
    // near cloud's edge has small curls on its billows and not one smooth brush-stroke outline.
    if (coarse) return saturate((mass - 0.5 * 0.45) / (1 - 0.5 * 0.45));
    float3 churn = p + wind * 0.33;
    float detail = Turned(churn, 1.39, 6.0, 0.11, lod) * 0.65 + Turned(churn, 2.71, 17.0, 0.53, lod) * 0.35;
    float erode = lerp(detail, 1 - detail, saturate(height * 3));
    return saturate((mass - erode * 0.45) / (1 - erode * 0.45));
}
// The shadow map's centre: the eye, snapped to a texel so the map does not shimmer as the camera moves.
float2 CloudShadowCentre(float3 eye)
{
    float texel = ShadowExtent / ShadowSize;
    return floor(eye.xz / texel) * texel;
}

// Where in the map a world point's cloud shadow is: along its ray to the sun up to the layer's base.
float2 CloudShadowUV(float3 world, float3 towardsSun, float2 centre)
{
    float3 entry = world + towardsSun * ((LayerBase - world.y) / max(towardsSun.y, 0.05));
    return (entry.xz - centre) / ShadowExtent + 0.5;
}
