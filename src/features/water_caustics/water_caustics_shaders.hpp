#pragma once

// HLSL for the water's caustics (water_caustics.hpp), compiled at run time with d3dcompiler_47.
namespace phantom_fx::water_caustics::shaders
{
// What a river's waves focus on the bed: the river's geometry again, right after its own draw, read through it. It reads what
// that draw left bound, after forward_scene::Common: the scene, the game's depth and the sun.
//
// Its output `o` lights the scene as scene x (1 + o): the sun's light on the bed, focused. Over the water and not under it: the
// game's river shows the bed through a copy of the scene taken before it (t3), so whatever is drawn before it is lost
// wherever the water is opaque. The webs are the bed's, though, so the fog in front of them takes them.
inline constexpr char Caustics[] = R"(
cbuffer cPSObject : register(b5)
{
    float4 m_objectRows[10];
    float4 m_sunColor; // Before the exposure
};

cbuffer Parameters : register(b6)
{
    float Intensity;  // The menu's
    float HasSunShadow;
    float2 Padding;
    float4 Scroll;    // Where the web's two layers have drifted to, in tiles
};

Texture2D<float> g_depth : register(t13);    // The scene behind the water: the bed
Texture2D<float> CausticsMap : register(t7); // Tiling (caustics_map.hpp)
Texture2D<float> SunShadow : register(t8);   // The rays' on the bed (ray_tracing::sunShadow); without them, none
SamplerState g_samplerLinear_Wrap : register(s10);

// How many metres each layer's tile spans: apart, so the tile's repetition does not read.
static const float FirstTile = 2.5f, SecondTile = 4.0f;

// Two layers, scrolled apart: their product keeps the crossings, where both carry a web. `lod` blurs it as the water
// deepens and the surface's focus spreads.
float CausticWeb(float2 world, float lod)
{
    const float a = CausticsMap.SampleLevel(g_samplerLinear_Wrap, world / FirstTile + Scroll.xy, lod);
    const float b = CausticsMap.SampleLevel(g_samplerLinear_Wrap, world / SecondTile + Scroll.zw, lod + 0.5f);

    return a * b * 2.5f;
}

// Every input up to the last read, in the river's order, so each lands in its register.
float4 main(float4 color : COLOR0, float4 position : SV_Position, float2 uv : TEXCOORD0, float3 basePosition : TEXCOORD1, float4 shadowPosition : TEXCOORD2,
    float4 viewPosition : TEXCOORD3, float3 worldPosition : TEXCOORD4) : SV_Target
{
    const float bedZ = SceneViewZ(g_depth, position.xy);
    const float column = max(bedZ - basePosition.z, 0.0f);

    // On the eye's ray through this pixel, a point's distance goes with its view z. Foggy weather raises m_eyepos above the
    // camera, but only its y, which this does not read.
    const float2 bed = lerp(m_eyepos.xz, worldPosition.xz, bedZ / basePosition.z);

    // Nothing focuses where the water meets the bed, the shore or anything standing in it, so the webs ramp in over the first
    // centimetres. They are strongest just under the surface, and deep water spreads what the waves focus until it is gone.
    const float contact = smoothstep(0.0f, 0.25f, column);
    const float depthFade = exp(-column / 5.0f);
    const float lod = lerp(0.0f, 3.5f, saturate(column / 8.0f));

    // The sun at the exposure is about 1.5 at noon, 0.3 in the rain and none at dusk and night (measured 2026-11-01), so the
    // webs are full by day and go with it.
    const float sun = saturate(dot(m_sunColor.rgb, float3(0.2126f, 0.7152f, 0.0722f)) * m_exposure.z);
    const float sunlit = HasSunShadow > 0.0f ? SunShadow.Load(int3(position.xy, 0)) : 1.0f;
    const float light = Intensity * contact * depthFade * sun * sunlit * FogTransmittance(position.xy, basePosition.z);

    return float4((light * CausticWeb(bed, lod)).xxx, 0.0f);
}
)";
}
