// Bindings and the denoiser's data access for the sun's ray-traced shadows (gi_shadow.hlsl,
// gi_shadow_filter.hlsl; docs/DEVELOPMENT.md, "Ray-traced shadows"). The denoiser is AMD's
// FidelityFX Shadow Denoiser (third_party/ffx_denoiser_shadows, MIT; licenses/AMD-FidelityFX-LICENSE.txt),
// whose Read, Write and Get functions this file provides.
#ifndef GI_SHADOW_HLSLI
#define GI_SHADOW_HLSLI

#include "gi_common.hlsli"

#define INVERTED_DEPTH_RANGE // Reversed-Z

RWStructuredBuffer<uint> g_Shadow_Masks : register(u80);        // A bit a pixel in 8x4 pixel tiles: 1 where the ray reached the sun
RWStructuredBuffer<uint> g_Shadow_TileMetadata : register(u81); // An 8x8 pixel tile each: TILE_META_DATA_*
RWTexture2D<float2> g_Shadow_Reprojection : register(u82);      // Mean and variance: after the temporal pass, then after the second filter
RWTexture2D<float2> g_Shadow_History : register(u83);           // After the first filter: the next frame's history
RWTexture2D<float3> g_Shadow_Moments[2] : register(u84);        // u84, u85: this frame's, the last frame's; mean, variance, samples
RWTexture2D<float> g_Shadow_Depths[2] : register(u86);          // u86, u87: this frame's depth, the last frame's
RWTexture2D<float> g_Shadow_Output : register(u88);             // What the sun pass reads: 1 where the sun reaches
int2 ShadowSize()
{
    return int2(g_Frame.dimensions.xy);
}

int2 ClampToShadow(int2 pixel)
{
    return clamp(pixel, 0, ShadowSize() - 1);
}

// ---- The denoiser's data access ----

int2 FFX_DNSR_Shadows_GetBufferDimensions()
{
    return ShadowSize();
}

float2 FFX_DNSR_Shadows_GetInvBufferDimensions()
{
    return 1.0f / float2(ShadowSize());
}

// Its linear depth is abs(z / w) of this times (ndc, depth, 1): a matrix whose z is 1 and whose w is
// 1 over toLinearDepth, as Fox Engine's projection is not kept on its own.
float4x4 FFX_DNSR_Shadows_GetProjectionInverse()
{
    float nearPlane = g_Frame.nearFar.x, farPlane = g_Frame.nearFar.y;
    return float4x4(1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f,
                    0.0f, 0.0f, (farPlane - nearPlane) / (nearPlane * farPlane), 1.0f / farPlane);
}

float4x4 FFX_DNSR_Shadows_GetViewProjectionInverse()
{
    return g_Frame.viewProjectionInverse;
}

// This frame's (ndc, depth, 1) to the last frame's clip space.
float4x4 FFX_DNSR_Shadows_GetReprojectionMatrix()
{
    return mul(g_Frame.previousViewProjection, g_Frame.viewProjectionInverse);
}

float3 FFX_DNSR_Shadows_GetEye()
{
    return g_Frame.eye.xyz;
}

uint FFX_DNSR_Shadows_IsFirstFrame()
{
    return g_Frame.shadowParams.w != 0.0f ? 1u : 0u;
}

float FFX_DNSR_Shadows_ReadDepth(int2 pixel)
{
    return g_Depth.Load(int3(ClampToShadow(pixel), 0));
}

float FFX_DNSR_Shadows_ReadPreviousDepth(int2 pixel)
{
    return g_Shadow_Depths[1][ClampToShadow(pixel)];
}

float3 FFX_DNSR_Shadows_ReadNormals(int2 pixel)
{
    return DecodeNormal(g_Normal.Load(int3(ClampToShadow(pixel), 0)));
}

bool FFX_DNSR_Shadows_IsShadowReciever(uint2 pixel)
{
    return all(pixel < uint2(ShadowSize())) && FFX_DNSR_Shadows_ReadDepth(int2(pixel)) > 0.0f;
}

// The motion from the last frame, in NDC, this frame's minus the last's: a moving mesh's from the game's
// velocity (which carries this frame's jitter, as the camera's motion here does), elsewhere the
// camera's, the depth's world position through the last frame's projection-view.
float2 FFX_DNSR_Shadows_ReadVelocity(int2 pixel)
{
    float2 moved;
    if (ObjectMotion(ClampToShadow(pixel), moved)) return moved * float2(2.0f, -2.0f) * FFX_DNSR_Shadows_GetInvBufferDimensions();

    float2 uv = (ClampToShadow(pixel) + 0.5f) * FFX_DNSR_Shadows_GetInvBufferDimensions();
    float2 ndc = 2.0f * float2(uv.x, 1.0f - uv.y) - 1.0f;
    float4 previous = mul(FFX_DNSR_Shadows_GetReprojectionMatrix(), float4(ndc, FFX_DNSR_Shadows_ReadDepth(pixel), 1.0f));
    return ndc - previous.xy / previous.w;
}

uint FFX_DNSR_Shadows_ReadRaytracedShadowMask(uint tile)
{
    return g_Shadow_Masks[tile];
}

void FFX_DNSR_Shadows_WriteMetadata(uint tile, uint mask)
{
    g_Shadow_TileMetadata[tile] = mask;
}

uint FFX_DNSR_Shadows_ReadTileMetaData(uint tile)
{
    return g_Shadow_TileMetadata[tile];
}

void FFX_DNSR_Shadows_WriteReprojectionResults(uint2 pixel, float2 meanAndVariance)
{
    g_Shadow_Reprojection[pixel] = meanAndVariance;
}

void FFX_DNSR_Shadows_WriteMoments(uint2 pixel, float3 moments)
{
    g_Shadow_Moments[0][pixel] = moments;
}

float3 FFX_DNSR_Shadows_ReadPreviousMomentsBuffer(int2 pixel)
{
    return g_Shadow_Moments[1][ClampToShadow(pixel)];
}

// The history is read bilinearly, as through the denoiser's linear sampler.
float FFX_DNSR_Shadows_ReadHistory(float2 uv)
{
    float2 position = uv * ShadowSize() - 0.5f;
    int2 corner = int2(floor(position));
    float2 weight = position - corner;
    float top = lerp(g_Shadow_History[ClampToShadow(corner)].x, g_Shadow_History[ClampToShadow(corner + int2(1, 0))].x, weight.x);
    float bottom = lerp(g_Shadow_History[ClampToShadow(corner + int2(0, 1))].x, g_Shadow_History[ClampToShadow(corner + int2(1, 1))].x, weight.x);
    return lerp(top, bottom, weight.y);
}

// ---- Where a pixel's shadow rays leave its surface ----
// A shadow ray's start towards `toLight`: from the surface's side facing the light, as leaves lit from
// behind take light through them.
void ShadowRayStart(PixelSurface surface, float3 toLight, out float3 origin, out float3 facing, out float tolerance)
{
    facing = dot(surface.normal, toLight) >= 0.0f ? surface.normal : -surface.normal;
    PixelRayStart(surface, facing, origin, tolerance);
}

// A hit as the probe lists it (GIShadowProbe::lightNearest); zeros for none.
uint4 ProbeHit(SceneHit hit)
{
    if (!hit.hit) return 0u;

    return uint4(hit.instance + 1, hit.geometry | (hit.frontFace ? 1u << 31 : 0u), InstanceKind(g_Instances[hit.instance]), asuint(hit.distance));
}

#endif
