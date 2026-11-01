#pragma once

// HLSL for the forward surfaces' caustics (forward_shadows.hpp), compiled at runtime with d3dcompiler_47.
namespace phantom_fx::forward_shadows::shaders
{
// The caustics a river puts on the bed under it: the same water geometry, drawn before the water blends over the
// scene, times the scene by the light its waves focus. The water then covers it, so the webs are read through the
// water, on the bed, not painted on the surface.
inline constexpr char Caustics[] = R"(
cbuffer Parameters : register(b0)
{
    float4 dimensions; // xy: the render size; z: the frame's index; w: the strength
};
// The river's own camera, its vertex shader's cVSScene (cb2 in every Prim_TppPrimRiver* and Prim_TppRiverModel*):
// the camera this draw is drawn with, so the pattern cannot lag it. Each matrix is applied a row at a time, as the
// vertex shader does (dp4 with the row).
cbuffer Scene : register(b1)
{
    float4 projectionViewRows[4]; // m_projectionView, rows 0-3: unread, it places the two below
    float4 projectionRows[4]; // m_projection, rows 4-7: view to clip
    float4 viewRows[4];       // m_view, rows 8-11: world to view, rigid
};
Texture2D<float> BedDepth : register(t0);     // The bed's depth, as the kick copied it: the water column above it
Texture2D<float> CausticsMap : register(t1);  // The art's caustic web, tiling
SamplerState CausticsSampler : register(s0);  // Linear, wrap

// How many metres one tile of the map spans.
static const float TileMetres = 2.5f;

// The view-space point at a pixel's NDC and depth: the projection's rows, each less the depth's share of w, are three
// planes through it (clip_i - ndc_i * clip_w = 0), solved by Cramer's rule.
float3 ViewAt(float2 ndc, float depth)
{
    const float4 a = projectionRows[0] - ndc.x * projectionRows[3];
    const float4 b = projectionRows[1] - ndc.y * projectionRows[3];
    const float4 c = projectionRows[2] - depth * projectionRows[3];
    const float3 bc = cross(b.xyz, c.xyz), ca = cross(c.xyz, a.xyz), ab = cross(a.xyz, b.xyz);

    return -(a.w * bc + b.w * ca + c.w * ab) / dot(a.xyz, bc);
}

// A view-space point back in the world: the view is rigid, so its inverse is its rotation's transpose.
float3 WorldOf(float3 view)
{
    const float3 relative = view - float3(viewRows[0].w, viewRows[1].w, viewRows[2].w);

    return relative.x * viewRows[0].xyz + relative.y * viewRows[1].xyz + relative.z * viewRows[2].xyz;
}

// Two layers of the map, scrolled apart and at different scales, so the tile's repetition does not read: the product
// keeps the crosses where both carry a web. `lod` blurs it as the water deepens and the surface's focus spreads.
float CausticWeb(float2 world, float time, float lod)
{
    const float2 first = world * (1.0f / TileMetres) + time * float2(0.015f, 0.007f);
    const float2 second = world * (1.0f / 4.0f) - time * float2(0.011f, 0.014f);
    const float a = CausticsMap.SampleLevel(CausticsSampler, first, lod).x;
    const float b = CausticsMap.SampleLevel(CausticsSampler, second, lod + 0.5f).x;

    return a * b * 2.5f;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const float2 uv = position.xy / dimensions.xy;
    const float2 ndc = float2(2 * uv.x - 1, 1 - 2 * uv.y);

    // The web is on the bed, not the surface: the bed's own world position anchors it, so it stays with the
    // bottom when the water moves over it.
    const float bed = BedDepth.Load(int3(int2(position.xy), 0));
    const float3 bedView = ViewAt(ndc, bed);
    const float3 bedWorld = WorldOf(bedView);

    // The water column under this fragment, along the view: the bed is farther than the surface.
    const float under = max(abs(bedView.z) - abs(ViewAt(ndc, position.z).z), 0.0f);

    // Nothing focuses where the water meets the bed, the shore or anything standing in it: the column goes to nothing,
    // so the contact ramps the webs in over the first few centimetres instead of cutting at the geometry's edge.
    // They are strongest just under the surface, and deep water spreads what the waves focus until it is gone.
    const float contact = smoothstep(0.0f, 0.25f, under);
    const float fade = exp(-under * (1.0f / 5.0f));
    const float lod = lerp(0.0f, 3.5f, saturate(under * (1.0f / 8.0f)));
    const float strength = dimensions.w * contact * fade;
    const float time = dimensions.z * (1.0f / 60.0f);
    const float shade = 1.0f + strength * CausticWeb(bedWorld.xz, time, lod);

    return float4(shade, shade, shade, 0.0f);
}
)";

}
