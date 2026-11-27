// In place of LocalReflectionAddBuffer (97a9ad7e0faca7b1594a38c331826a94), rebuilt from its disassembly: a reflection
// blended over the scene by its specular intensity and the pass's strength, then sRGB-encoded as the game writes the
// scene. On the G-buffer's surfaces the reflection comes from the rays: reflections.hlsl gives each hit's light by the
// game's Fresnel and its blend, and here it fades as LocalReflectionTPP fades its own (by the ray's way across the
// screen, against the march's reach, and the fog), but for the screen's edges, as a ray needs none. Only where a
// surface was drawn in front of the G-buffer's (the scene's depth nearer than the G-buffer's), water, which the rays
// do not see, does it stay the screen-space map. Glass there has reflections of its own (forward_surfaces.hpp): the map, the
// surface's behind it, leaves it.
cbuffer cPSSystem : register(b0)
{
    float4 m_param;
    float4 m_renderInfo;
    float4 m_renderBuffer;
    float4 m_dominantLightDir;
};

cbuffer Parameters : register(b1)
{
    float Reflections; // The menu's strength, on the rays' alone
    float3 Padding;
};

cbuffer cPSScene : register(b2)
{
    float4 m_sceneRows[21];
    float4 m_projectionParam;
    float4 m_sceneRows2[3];
    float4 m_fogParam1; // x: 1 / log2 of the fog's far distance
};

// The blend's strength, the roughness's level-of-detail bias and the march's reach are its c12.xyz.
cbuffer cPSObject : register(b5)
{
    float4x4 m_viewWorld;
    float4x4 m_world;
    float4 m_useWeightCount;
    float4 m_localParam[4];
};

Texture2D<float4> inColorImage : register(t0);
Texture2D<float4> inRefMapImage : register(t1);
Texture2D<float4> inMaterialImage : register(t2);
Texture2D<float> SceneDepth : register(t3);      // The depth the reflections' mask read, with the surfaces drawn after the G-buffer
Texture2D<float> SurfaceDepth : register(t4);    // The G-buffer's depth, as the ray-traced scene's kick packed it
Texture2D<float4> ReflectionMap : register(t5);  // The rays' hits (ray_tracing::reflectionMap)
Texture2D<float4> Reflection : register(t6);     // Its alpha: how far across the screen a ray's hit is (ray_tracing::reflection)
Texture2D<float4> SurfaceWeights : register(t7); // Its alpha: how much of a pixel glass covers (forward_surfaces::surfaceWeights)
Texture2D<float4> g_tex_fog : register(t12);     // The game's, still bound from its screen-space reflections
SamplerState g_samplerPoint_Wrap : register(s8);
SamplerState g_samplerLinear_Wrap : register(s10);

// The fog at a pixel as LocalReflectionTPP's GetVolumetricFog reads it: alpha, between two of the 128 tiles a slice.
float FogAt(float2 pixel, float depth)
{
    const float viewZ = m_projectionParam.z / (depth - m_projectionParam.w);
    const float layer = saturate(log2(viewZ) * m_fogParam1.x) * 127;
    const float2 clip = (pixel - 0.5) / m_renderInfo.xy * float2(2, -2) + float2(-1, 1);
    const float2 uv = clip * float2(0.014648, 0.123047) + float2(0.015625, 0.125);
    const float2 slices = float2(floor(layer), floor(min(layer + 1, 127))) / 32;
    const float2 column = frac(slices) * 32 * 0.03125, row = floor(slices) * 0.25;
    const float near = g_tex_fog.Sample(g_samplerLinear_Wrap, uv + float2(column.x, row.x)).a;
    const float far = g_tex_fog.Sample(g_samplerLinear_Wrap, uv + float2(column.y, row.y)).a;
    return lerp(near, far, frac(layer));
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const float2 uv = (position.xy - 0.5 + 0.496094) * m_renderBuffer.zw;
    const float2 material = inMaterialImage.Sample(g_samplerPoint_Wrap, uv).xy; // Roughness, specular intensity
    const float3 color = inColorImage.Sample(g_samplerLinear_Wrap, uv).rgb;

    // Reversed Z: a surface in front of the G-buffer's has the greater depth.
    const int3 pixel = int3(position.xy, 0);
    const float surfaceDepth = SurfaceDepth.Load(pixel);
    float4 reflected;
    float strength = m_localParam[3].x;
    if (SceneDepth.Load(pixel) > surfaceDepth * 1.000001 + 1e-9)
    {
        reflected = SurfaceWeights.Load(pixel).a > 0 ? 0 : inRefMapImage.SampleLevel(g_samplerLinear_Wrap, uv, saturate(material.x + m_localParam[3].y) * 5);
    }
    else
    {
        const float fog = FogAt(position.xy, surfaceDepth);
        const float fog2 = fog * fog, fog4 = fog2 * fog2;
        reflected = ReflectionMap.Load(pixel) * saturate(1 - Reflection.Load(pixel).a / (0.5 * m_localParam[3].z)) * (fog4 * fog4);
        strength *= Reflections;
    }

    const float3 reflectedColor = material.y * reflected.rgb * strength;
    const float weight = saturate(material.y * reflected.a * strength);
    const float3 blended = color * (1 - weight) + weight * reflectedColor;
    const float3 encoded = blended <= 0.003131 ? blended * 12.92 : 1.055 * pow(max(blended, 0.00001), 1 / 2.4) - 0.055;
    return float4(encoded, 1);
}
