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
};

SamplerState LinearWrap : register(s10);
// The add-on's 64-cube of value-noise fbm (clouds.cpp), bound over the game's texture at t14 for the draw.
Texture3D<float> CloudNoise : register(t14);

float Noise3D(float3 p)
{
    return CloudNoise.SampleLevel(LinearWrap, p, 0).x;
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
    float weather = pow(saturate(domeAlpha * m_materials[1].w), 1.5);
    // Below 100% the menu thins the weather's clouds; above, it adds cloud towards an overcast, so a clear day can still have a
    // full sky (a multiplier on a clear day's small coverage could not).
    float more = saturate(g_cloudCoverage - 1);
    return saturate(weather * min(g_cloudCoverage, 1) + more * (1 - weather) * 0.6);
}

// The density at one point of the world. Everything (the weather's coverage, the column's top, the shape) is looked
// up HERE, at this point, so the outline is a 3D shape and not a picture on the layer's base plane, and the march
// towards the sun sees where a neighbouring cloud begins and ends.
float CloudDensity(float3 where, float2 windXZ, float3 wind, float baseCover)
{
    float height = saturate((where.y - LayerBase) / (LayerTop - LayerBase));
    // Larger clouds from the same volume: a smaller frequency, so a cell spans more of the sky. Anchored on the world, so a cloud
    // stays where it is as the camera moves.
    float frequency = 1.0 / clamp(g_cloudSize, 0.25, 4.0);
    float group = Noise3D(float3(where.x * 0.00005 * frequency + windXZ.x, 0.5, where.z * 0.00005 * frequency + windXZ.y));
    // The region thins or thickens the cloud, never empties it: a hard gate left whole views clear whatever the coverage.
    float cover = baseCover * lerp(0.35, 1.0, smoothstep(0.2, 0.7, group));
    if (cover <= 0) return 0;
    float topFraction = 0.45 + 0.55 * group;
    // Larger cells than before: one cloud is kilometres across, as a cumulus is.
    float3 p = where * (0.00022 * frequency) + wind;
    p.y *= 1.6;
    float base = Noise3D(p) * 0.65 + Noise3D(p * 2.3 + 0.37) * 0.35;
    float columnTop = saturate(topFraction + 0.5 * (base - 0.5));
    float profile = smoothstep(0, 0.05, height) * (1 - smoothstep(columnTop * 0.6, columnTop, height));
    if (profile <= 0) return 0;
    // A cumulus narrows as it rises: the coverage tightens with height, so the top is a dome over a broad, flat base.
    float shapeCover = saturate(cover * 2.0) * lerp(1.0, 0.6, saturate(height / max(columnTop, 0.05)));
    float mass = saturate((base - (1 - shapeCover)) / max(shapeCover, 0.08)) * profile;
    if (mass <= 0) return 0;
    // Erosion only at the rim: ragged wisps low down, rounded billows higher up.
    float detail = Noise3D(p * 6.0 + wind * 2);
    float erode = lerp(detail, 1 - detail, saturate(height * 3));
    return saturate((mass - erode * 0.35) / (1 - erode * 0.35));
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
