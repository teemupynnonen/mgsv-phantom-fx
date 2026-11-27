// What every fog pass reads: the add-on's constants, copies of the constant buffers the game's fog
// pass (VolFog_TppVolFog) and sun pass bound, so the fog follows the game's weather and time of day,
// and the froxels' places.
cbuffer Fog : register(b0)
{
    uint2 TileSize;      // Texels of one depth slice in the game's fog texture
    uint Shadowed;       // 1 when the sun pass's shadow map and constants are this frame's
    uint ShadowSamples;  // Shadow comparisons per froxel
    float2 Jitter;       // This frame's place in each froxel, across the view, in froxels from -0.5 to 0.5
    float DepthJitter;   // And through its slice, from 0 to 1
    float HistoryWeight; // Of the last frames' visibility, reprojected; 0 without
    uint Physical;       // 1: the physical look's air and height fog in place of the game's formula
    uint SkySun;         // 1: SkyMaterial holds a sky draw's constants
    float Haze;          // The physical look's haze, 0 to 1: how much of the game's fog its height fog takes
    float Padding;
};

cbuffer FogScene : register(b1) // cPSScene of the game's fog pass
{
    float4x4 FogProjectionView;
    float4x4 FogProjection;
    float4 FogPlacement[4];      // m_view, by which the fog pass places its froxels
    float4x4 FogShadowProjection;
    float4x4 FogShadowProjection2;
    float4 FogEyePosition;
    float4 FogProjectionParam;   // 1 / P00, 1 / P11, B, A
    float4 FogViewportSize;
    float4 FogExposure;
    float4 FogParam[3];          // [1]: x 1 / log2 of the fog's far distance, y the normalising factor
    float4 FogColor;
    float4 FogCameraCenterOffset;
    float4 FogShadowMapResolutions;
};

cbuffer FogMaterial : register(b2) // cPSMaterial of the game's fog pass
{
    float4 FogMedium[8];
};

cbuffer SunScene : register(b5) // cPSScene of the sun pass
{
    float4x4 m_projectionView;
    float4x4 m_projection;
    float4 m_view[4];               // Rows: view to world space (the camera's position in w)
    float4 m_shadowProjection[4];   // Rows: view position to the first cascade
    float4 m_cascades[4];           // m_shadowProjection2: columns x, y, z place cascades 1 to 3 (offset, scale in row 3)
    float4 m_eyepos;                // The camera in the world, metres, y up
    float4 m_projectionParam;
    float4 m_viewportSize;
    float4 m_exposure;
    float4 m_fogParam[3];
    float4 m_fogColor;
    float4 m_cameraCenterOffset;
    float4 m_shadowMapResolutions;
};

cbuffer SunLight : register(b6) // cPSLight of the sun pass: [0] towards the sun, view space; [1] its colour
{
    float4 m_lightParams[11];
};

static const uint Slices = 128;

// The view-space direction (z 1) of a place in a slice tile, in texels, as the game's readers sample
// the fog: uv = clip x (0.014648, 0.123047) + (0.015625, 0.125) across the 32 x 4 tiles.
float3 ColumnDirection(float2 texel)
{
    float2 clip = ((texel + 0.5) / TileSize - 0.5) / float2(0.46875, 0.4921875);
    return float3(clip * FogProjectionParam.xy, 1);
}

// Slice depths in the view run from 1 m to the far distance, evenly in log2.
float SliceDepth(float slice)
{
    return exp2(slice / (Slices - 1) / FogParam[1].x);
}

// A view-space direction in the world, y up.
float3 ToWorld(float3 direction)
{
    return float3(dot(m_view[0].xyz, direction), dot(m_view[1].xyz, direction), dot(m_view[2].xyz, direction));
}

