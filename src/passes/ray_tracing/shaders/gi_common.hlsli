// Bindings and helpers shared by the global illumination's kernels (docs/DEVELOPMENT.md, "Global
// illumination"). The maths helpers are AMD's, from Capsaicin's GI-1.2 (MIT; licenses/Capsaicin-LICENSE.txt).
#ifndef GI_COMMON_HLSLI
#define GI_COMMON_HLSLI

#include "gi_shared.h"

// ---- Bindings: one root signature for every kernel (gi_renderer.cpp) ----

ConstantBuffer<GIFrameConstants> g_Frame : register(b0);
cbuffer DispatchConstants : register(b1)
{
    uint4 g_Dispatch; // Per dispatch: what each kernel says it is
};

RaytracingAccelerationStructure g_Scene : register(t0, space1);

// The frame's inputs, packed by Direct3D 11 from the game's G-buffer.
Texture2D<float> g_Depth : register(t0);    // Reversed-Z, 0 where the sky is
Texture2D<float4> g_Normal : register(t1);  // World space as 0.5 n + 0.5; 0 where the sky is
Texture2D<float4> g_Albedo : register(t2);  // Linear
StructuredBuffer<uint> g_NewGeometries : register(t4); // The frame's new geometries' rows of the table (counts.x)
// The frame's instances as the top level holds them, and the scene's geometry (gi_scene.hpp): the
// pool, vertices of 16 bytes (xyz the position, object space; w the texture coordinates, two halves,
// with GI_GEOMETRY_UV) and 16-bit indices; the table of geometries, an instance's from its first.
StructuredBuffer<GIInstanceDesc> g_Instances : register(t5);
ByteAddressBuffer g_GeometryPool : register(t6);
StructuredBuffer<GIGeometry> g_Geometries : register(t16);
// The game's textures the scene holds (gi_textures.hpp), each its levels of detail from the first no
// larger than the scene keeps, viewed as sRGB where the game's are (GIGeometry::texture).
Texture2D<float4> g_SceneTextures[GI_MAX_TEXTURES] : register(t0, space2);

RWTexture2D<float4> g_Output : register(u0);
RWByteAddressBuffer g_GeometryPoolOut : register(u1); // The pool, while the frame's terrain and characters' poses go in
RWStructuredBuffer<uint> g_Counters : register(u4);
RWStructuredBuffer<uint4> g_DispatchArgs : register(u5);
RWStructuredBuffer<uint4> g_GeometryAlbedo : register(u6); // Sums of linear albedo x 255 and their count, by geometry

// The game's velocity for its moving meshes (characters, the horse, trees in the wind) as its velocity
// passes drew it this frame, when it came in time (objectMotion.x): R 1 where one drew, B and A the
// screen motion from the last frame to this one in pixels / 64, y down, stored as 0.5 v + 0.5 in 8 bits
// (docs/DEVELOPMENT.md, "Velocity"). It carries the frame's jitter, as the camera's motion does here.
Texture2D<float4> g_ObjectVelocity : register(t17);

// Path tracing's hits (gi_hit_light.hlsli): each row of the game's material table as its specular colour at normal
// incidence (the guides' pass, ray_tracing.cpp); and the sky as the game draws it, from every side (sky_capture.hpp),
// in the same units as the sky's light.
Texture2D<float4> g_MaterialSpecular : register(t23);
TextureCube<float4> g_SkyCube : register(t24);

SamplerState g_PointSampler : register(s0);
SamplerState g_LinearSampler : register(s1);
SamplerState g_WrapSampler : register(s2);

// ---- Constants ----

#define TWO_PI      6.283185307179586476925286766559f
#define FLT_MAX     3.402823466e+38f

// ---- Maths (Capsaicin) ----

float squared(float v) { return v * v; }

// NaNs and infinities out: through x / (1 + x), saturated, and back.
float3 RemoveNaNs(float3 color)
{
    color /= (1.0f + color);
    color = saturate(color);
    return color / max(1.0f - color, 1e-4f);
}

float luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

uint pcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint xxHash(uint value)
{
    const uint prime32_2 = 2246822519u, prime32_3 = 3266489917u;
    const uint prime32_4 = 668265263u, prime32_5 = 374761393u;
    uint ret = value + prime32_5;
    ret = prime32_4 * ((ret << 17) | (ret >> (32 - 17)));
    ret = prime32_2 * (ret ^ (ret >> 15));
    ret = prime32_3 * (ret ^ (ret >> 13));
    return ret ^ (ret >> 16);
}

uint2 packHalf4(float4 value)
{
    return uint2(f32tof16(value.x) | (f32tof16(value.y) << 16), f32tof16(value.z) | (f32tof16(value.w) << 16));
}

float4 unpackHalf4(uint2 packed)
{
    return float4(f16tof32(packed.x & 0xFFFFu), f16tof32(packed.x >> 16), f16tof32(packed.y & 0xFFFFu), f16tof32(packed.y >> 16));
}

uint packNormal(float3 value)
{
    int3 packed = int3(clamp(value, -1.0f, 1.0f) * 511.0f + (0.5f * sign(value))) & 0x3FF;
    return packed.x | (packed.y << 10) | (packed.z << 20);
}

float3 unpackNormal(uint packed)
{
    int3 value = int3(packed << 22, packed << 12, packed << 2) >> 22;
    return float3(value) * (1.0f / 511.0f);
}

void GetOrthoVectors(float3 n, out float3 b1, out float3 b2)
{
    bool sel = abs(n.z) > 0;
    float3 p2 = sel ? n : n.zyx;
    float k = 1.0f / sqrt(squared(p2.z) + squared(n.y));
    b1 = float3(0.0f, -p2.z * k, n.y * k);
    b1 = sel ? b1 : b1.zyx;
    b2 = cross(n, b1);
}

// Cosine-weighted direction around `normal` (Shirley).
float3 mapToCosineHemisphere(float2 samples, float3 normal)
{
    float3 u, v;
    GetOrthoVectors(normal, u, v);

    float phi = TWO_PI * samples.y;
    float sinPhi, cosPhi;
    sincos(phi, sinPhi, cosPhi);
    float sinTheta = sqrt(samples.x);
    float cosTheta = sqrt(1.0f - samples.x);
    return normalize(u * sinTheta * cosPhi + v * sinTheta * sinPhi + normal * cosTheta);
}

// A uniform point on a disc of `radius` around the unit `axis`, from its centre.
float3 DiscOffset(float3 axis, float radius, float2 samples)
{
    float3 u, v;
    GetOrthoVectors(axis, u, v);

    float r = radius * sqrt(samples.x);
    float angle = TWO_PI * samples.y;
    return r * (cos(angle) * u + sin(angle) * v);
}

float3 transformPointProjection(float3 position, float4x4 transform)
{
    float4 result = mul(transform, float4(position, 1.0f));
    return result.xyz / result.w;
}

// The world position of a pixel's texture coordinate and depth.
float3 transformPointProjection(float2 uv, float depth, float4x4 transform)
{
    return transformPointProjection(float3(2.0f * float2(uv.x, 1.0f - uv.y) - 1.0f, depth), transform);
}

// The world position of a render pixel's centre at a depth.
float3 DepthPosition(uint2 pixel, float depth)
{
    return transformPointProjection((pixel + 0.5f) / g_Frame.dimensions.xy, depth, g_Frame.viewProjectionInverse);
}

// Distance along the view of a reversed-Z depth with a finite far plane, as Fox Engine's.
float toLinearDepth(float depth, float2 nearFar)
{
    return (nearFar.x * nearFar.y) / (nearFar.x + depth * (nearFar.y - nearFar.x));
}

float CalculateHaltonNumber(uint index, uint base)
{
    float f = 1.0f, result = 0.0f;
    for (uint i = index; i > 0;)
    {
        f /= base;
        result += f * (i % base);
        i = uint(i / float(base));
    }

    return result;
}

float2 CalculateHaltonSequence(uint index)
{
    return float2(CalculateHaltonNumber((index & 0xFFu) + 1, 2), CalculateHaltonNumber((index & 0xFFu) + 1, 3));
}

// Capsaicin's PCG generator, seeded by hashing in place of its seed buffer.
struct Random
{
    uint state;

    uint randInt()
    {
        uint previous = state;
        state = state * 747796405u + 2891336453u;
        uint word = ((previous >> ((previous >> 28u) + 4u)) ^ previous) * 277803737u;
        return (word >> 22u) ^ word;
    }

    uint randInt(uint maximum)
    {
        return randInt() % maximum;
    }

    float rand()
    {
        return float(randInt() >> 8) * (1.0f / 16777216.0f);
    }

    float2 rand2()
    {
        return float2(rand(), rand());
    }
};

Random MakeRandom(uint seed, uint index)
{
    const uint increment = (index << 1) | 1u;
    Random result;
    result.state = (pcgHash(seed) + increment) * 747796405u + increment;
    return result;
}

// Interleaved gradient noise (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare", SIGGRAPH 2014): a number in [0, 1) per pixel that differs most between neighbours, moving
// on each frame as its temporal form does.
float InterleavedGradientNoise(uint2 pixel, uint frame)
{
    float2 p = float2(pixel) + 5.588238f * float(frame & 63u);
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// Point `index` of Roberts' R2 sequence: 2D points that fill the unit square evenly however many are taken.
float2 R2(uint index)
{
    return frac(float(index) * float2(0.7548776662466927f, 0.5698402909980532f));
}

// ---- Spherical harmonics (Capsaicin) ----

void SH_GetCoefficients(float3 direction, out float coefficients[9])
{
    float pz2 = direction.z * direction.z;
    coefficients[0] = 0.2820947917738781f;
    coefficients[2] = 0.4886025119029199f * direction.z;
    coefficients[6] = 0.9461746957575601f * pz2 + -0.3153915652525201f;
    float fC0 = direction.x, fS0 = direction.y;
    float fTmpA = -0.48860251190292f;
    coefficients[3] = fTmpA * fC0;
    coefficients[1] = fTmpA * fS0;
    float fTmpB = -1.092548430592079f * direction.z;
    coefficients[7] = fTmpB * fC0;
    coefficients[5] = fTmpB * fS0;
    float fC1 = direction.x * fC0 - direction.y * fS0;
    float fS1 = direction.x * fS0 + direction.y * fC0;
    float fTmpC = 0.5462742152960395f;
    coefficients[8] = fTmpC * fC1;
    coefficients[4] = fTmpC * fS1;
}

// ---- The game's motion ----

// Whether a moving mesh of the game's covers a render pixel this frame, and its screen motion from the
// last frame to this one, in pixels, y down. The game clamps its velocity to 64 pixels; beyond that only
// the camera's motion is known.
bool ObjectMotion(int2 pixel, out float2 moved)
{
    moved = 0.0f;
    if (g_Frame.objectMotion.x == 0u) return false;

    float4 velocity = g_ObjectVelocity.Load(int3(pixel, 0));
    moved = (velocity.ba * 2.0f - 1.0f) * 64.0f;
    return velocity.r > 0.5f && length(moved) < 63.0f;
}

// The upscaler's jitter (NDC) as a shift on screen, in uv.
float2 JitterUv(float2 ndc)
{
    return float2(0.5f, -0.5f) * ndc;
}

// A render pixel's camera motion (the last frame's uv minus this one's, without the jitter) and its view
// depth last frame: its unjittered clip position through the one matrix built in double from the
// projections and views apart (camera::previousFromCurrentByParts), as the upscaler's motion vectors.
// Through its world position and the game's float projection-views the motion was off by up to 0.19 render
// pixels near the camera on a still frame, changing with each jitter (2026-10-29).
void CameraMotionOf(uint2 pixel, float depth, out float2 motion, out float previousViewZ)
{
    float2 uv = (pixel + 0.5f) / g_Frame.dimensions.xy;
    float4 unjittered = float4(float2(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y) - g_Frame.jitter.xy, depth, 1.0f);
    float4 last = mul(g_Frame.previousFromCurrent, unjittered);
    motion = (float2(0.5f, -0.5f) * last.xy / last.w + 0.5f) - (float2(0.5f, -0.5f) * unjittered.xy + 0.5f);
    previousViewZ = last.w / dot(g_Frame.clipToViewW, unjittered);
}

// ---- The scene ----

// The sky's radiance in a world direction, from the game's own sky light (gi_renderer.cpp).
float3 SkyRadiance(float3 direction)
{
    float coefficients[9];
    SH_GetCoefficients(direction, coefficients);

    float3 radiance = 0.0f;
    for (uint i = 0; i < 9; ++i) radiance += coefficients[i] * g_Frame.skySH[i].xyz;

    return max(radiance, 0.0f);
}

// The sky a reflection sees in a world direction: the one the game draws, with its horizon, clouds and sun's glow, once
// captured. Diffuse rays keep the sky's light (SkyRadiance): it is what the game's ambient is tuned to.
float3 SkySeen(float3 direction)
{
    if (g_Frame.reconstruction.z) return g_SkyCube.SampleLevel(g_LinearSampler, direction, 0.0f).rgb;

    return SkyRadiance(direction);
}

float3 DecodeNormal(float4 encoded)
{
    return normalize(2.0f * encoded.xyz - 1.0f);
}

// Rays leave a surface the depth buffer gave from slightly in front of it: the scene's triangles are
// not exactly where the game drew them (another level of detail, rounding).
float3 OffsetFromScreenSurface(float3 position, float3 normal)
{
    float distanceToEye = distance(position, g_Frame.eye.xyz);
    return position + normal * (0.02f + 0.002f * distanceToEye);
}

// ---- The scene's geometry ----

// An instance's kind (GI_INSTANCE_*), and its first geometry's row of the table.
uint InstanceKind(GIInstanceDesc instance)
{
    return instance.idAndMask & GI_INSTANCE_KINDS;
}

uint FirstGeometry(GIInstanceDesc instance)
{
    return instance.hitGroupAndFlags & 0xFFFFFFu;
}

// A triangle's corners in its geometry.
uint3 TriangleIndices(GIGeometry geometry, uint primitive)
{
    if (geometry.indexOffset == GI_NO_INDICES) return 3 * primitive + uint3(0, 1, 2);

    // Three 16-bit indices from the four-byte word holding the first.
    uint address = geometry.indexOffset + 6 * primitive;
    uint2 words = g_GeometryPool.Load2(address & ~3u);
    return (address & 2u) ? uint3(words.x >> 16, words.y & 0xFFFFu, words.y >> 16) : uint3(words.x & 0xFFFFu, words.x >> 16, words.y & 0xFFFFu);
}

float4 PoolVertex(GIGeometry geometry, uint index)
{
    return asfloat(g_GeometryPool.Load4(geometry.vertexOffset + 16 * index));
}

float3 ToWorld(GIInstanceDesc instance, float3 position)
{
    float4 p = float4(position, 1.0f);
    return float3(dot(instance.transform[0], p), dot(instance.transform[1], p), dot(instance.transform[2], p));
}

// A direction under an instance's transform, not normalised.
float3 ToWorldDirection(GIInstanceDesc instance, float3 direction)
{
    return float3(dot(instance.transform[0].xyz, direction), dot(instance.transform[1].xyz, direction), dot(instance.transform[2].xyz, direction));
}

// A vertex's normal and tangent, w the binormal's side (GIGeometry::normalOffset).
void VertexFrame(GIGeometry geometry, uint index, out float3 normal, out float4 tangent)
{
    uint4 words = g_GeometryPool.Load4(geometry.normalOffset + 16 * index);
    normal = float3(f16tof32(words.x), f16tof32(words.x >> 16), f16tof32(words.y));
    tangent = float4(f16tof32(words.z), f16tof32(words.z >> 16), f16tof32(words.w), f16tof32(words.w >> 16));
}

// A triangle of an instance's geometry: its corners in world space, and the texture coordinates
// where the ray crossed it (with GI_GEOMETRY_UV).
struct SceneTriangle
{
    float3 v0, v1, v2;
    GIGeometry geometry;
    uint row; // The geometry's row of the table
    uint3 indices;
};

SceneTriangle LoadTriangle(uint instanceIndex, uint geometryIndex, uint primitive)
{
    GIInstanceDesc instance = g_Instances[instanceIndex];
    SceneTriangle t;
    t.row = FirstGeometry(instance) + geometryIndex;
    t.geometry = g_Geometries[t.row];
    t.indices = TriangleIndices(t.geometry, primitive);
    t.v0 = ToWorld(instance, PoolVertex(t.geometry, t.indices.x).xyz);
    t.v1 = ToWorld(instance, PoolVertex(t.geometry, t.indices.y).xyz);
    t.v2 = ToWorld(instance, PoolVertex(t.geometry, t.indices.z).xyz);
    return t;
}

// The point at DXR's barycentrics (the weights of v1 and v2).
float3 TrianglePoint(SceneTriangle t, float2 barycentrics)
{
    return (1.0f - barycentrics.x - barycentrics.y) * t.v0 + barycentrics.x * t.v1 + barycentrics.y * t.v2;
}

// Texture coordinates in the pool at `offset`: two halves.
float2 PoolUV(uint offset)
{
    uint packed = g_GeometryPool.Load(offset);
    return f16tof32(uint2(packed, packed >> 16));
}

float2 VertexUV(GIGeometry geometry, uint index)
{
    return PoolUV(geometry.vertexOffset + 16 * index + 12);
}

// A vertex's coordinates on its mesh's layer (GIGeometry::layerMapping).
float2 VertexLayerUV(GIGeometry geometry, uint index)
{
    return (PoolUV(geometry.layerUvOffset + 4 * index) + geometry.layerMapping.zw) * geometry.layerMapping.xy;
}

// A vertex's coordinates on its mesh's layer's mask: the first coordinates, or its own.
float2 VertexLayerMaskUV(GIGeometry geometry, uint index)
{
    return geometry.layerMaskUvOffset ? PoolUV(geometry.layerMaskUvOffset + 4 * index) : VertexUV(geometry, index);
}

// The coordinates at DXR's barycentrics, from those at a triangle's corners.
float2 InterpolatedUV(float2 uv0, float2 uv1, float2 uv2, float2 barycentrics)
{
    return (1.0f - barycentrics.x - barycentrics.y) * uv0 + barycentrics.x * uv1 + barycentrics.y * uv2;
}

float2 TriangleUV(GIGeometry geometry, uint3 indices, float2 barycentrics)
{
    return InterpolatedUV(VertexUV(geometry, indices.x), VertexUV(geometry, indices.y), VertexUV(geometry, indices.z), barycentrics);
}

// The level of detail at which a ray cone `footprint` metres across meets a triangle (Akenine-Moller
// et al., "Texture Level of Detail Strategies for Real-Time Ray Tracing", Ray Tracing Gems chapter 20):
// the texels across the footprint, from the triangle's texels per square metre.
float TextureLod(float3 v0, float3 v1, float3 v2, float2 uv0, float2 uv1, float2 uv2, float2 size, float footprint)
{
    float worldArea = length(cross(v1 - v0, v2 - v0));
    float texelArea = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y)) * size.x * size.y;
    return log2(max(footprint * sqrt(texelArea / max(worldArea, 1e-12f)), 1e-6f));
}

// The level of detail of a cut-out's or a pane's texture where a ray's cone is `footprint` metres across, from the mesh's
// average texels a metre (GIGeometry::texelsPerMetre); its sharpest at 0. A jungle's rays run this about 20 million times a
// frame: working it out from each triangle's vertices and placement took 2 ms of that, and showed no different on the same
// frozen frame.
float MeshTextureLevel(GIGeometry geometry, float footprint)
{
    return footprint > 0.0f ? log2(max(footprint * geometry.texelsPerMetre, 1e-6f)) : 0.0f;
}

// Whether a cut-out geometry (non-opaque, as glass is) stops a ray where the ray crosses it: with its
// texture, where the texture's alpha is at least the game's threshold at the level of detail the ray's
// cone (`footprint` metres across there) meets (MeshTextureLevel); without, as `untextured` says. Glass never does:
// a ray towards a light passes it dimmed by its cover (PaneTransmittance), and only a reflection's rays meet it
// (TraceClosest's `canMeetGlass`).
bool CutOutBlocks(uint row, uint primitive, float2 barycentrics, bool untextured, float footprint)
{
    GIGeometry geometry = g_Geometries[row];
    if (geometry.flags & GI_GEOMETRY_GLASS) return false;
    if ((geometry.flags & GI_GEOMETRY_ALPHA_TEXTURE) == 0) return untextured;

    float2 uv = TriangleUV(geometry, TriangleIndices(geometry, primitive), barycentrics);
    Texture2D<float4> texture = g_SceneTextures[NonUniformResourceIndex(geometry.texture - 1)];
    float4 texel = texture.SampleLevel(g_WrapSampler, uv, MeshTextureLevel(geometry, footprint));
    return texel.a >= geometry.alphaThreshold;
}

// A pane's cover in a texel of its mask (fox3dfw_glass*'s): a colour texture holds it in alpha, a plain one, whose alpha
// is 1, in red. Its dirt and frosting are cover: they are what the game's draw shows of the pane.
float GlassCover(float4 texel)
{
    return texel.a < 1.0f ? texel.a : texel.r;
}

// What a pane's mask covers on its triangle `indices` where a ray crosses it, read over a patch `footprint` metres wide
// (0: its sharpest level); nothing for a pane without its mask.
float PaneCover(GIGeometry geometry, uint3 indices, float2 barycentrics, float footprint)
{
    if (!geometry.texture || !(geometry.flags & GI_GEOMETRY_UV)) return 0.0f;

    float2 uv = TriangleUV(geometry, indices, barycentrics);
    Texture2D<float4> texture = g_SceneTextures[NonUniformResourceIndex(geometry.texture - 1)];
    return GlassCover(texture.SampleLevel(g_WrapSampler, uv, MeshTextureLevel(geometry, footprint)));
}

// What clean glass lets through of a ray meeting it at `cosine` to its normal: what neither of its two faces reflects,
// by Schlick's approximation of glass's Fresnel reflectance (F0 0.04, n = 1.5). 92% head-on, a third at 80 degrees, as a
// low sun through a window.
float ClearGlassTransmittance(float cosine)
{
    float reflected = 0.04f + 0.96f * pow(1.0f - saturate(cosine), 5.0f);
    return (1.0f - reflected) * (1.0f - reflected);
}

// The cosine between a ray along `objectDirection`, in the pane's object space, and its triangle `indices`' normal.
float PaneCosine(GIGeometry geometry, uint3 indices, float3 objectDirection)
{
    float3 v0 = PoolVertex(geometry, indices.x).xyz;
    float3 normal = cross(PoolVertex(geometry, indices.y).xyz - v0, PoolVertex(geometry, indices.z).xyz - v0);
    return abs(dot(normal, objectDirection)) * rsqrt(max(dot(normal, normal) * dot(objectDirection, objectDirection), 1e-20f));
}

// How much of the light a texel the mask covers takes. Dust and grime absorb some and scatter some back, and scatter
// the rest on through, which a ray towards a light counts as passing: a covered texel is no wall. A judgement, not a
// measurement: the game's masks say how much of a pane is dirty, not how dense the dirt is.
static const float DirtLoss = 0.5f;

// What a pane lets through of a ray towards a light that crosses its triangle `indices` at `cosine` to its normal: what
// the clean glass does not reflect (ClearGlassTransmittance), less what its dirt takes (DirtLoss). The mask alone gives the
// dirt, not its row of the game's material table as a reflection's ray takes it (HitMaterialOf): that row is the
// specular's, and a pane whose row is empty or unread (its F0 0) would show no dirt at all. A mirror lets it all through.
float PaneTransmittance(GIGeometry geometry, uint3 indices, float2 barycentrics, float footprint, float cosine)
{
    if (geometry.flags & GI_GEOMETRY_MIRROR) return 1.0f;

    return ClearGlassTransmittance(cosine) * (1.0f - DirtLoss * PaneCover(geometry, indices, barycentrics, footprint));
}

// Every ray's flags. The terrain is one-sided, as the game draws it: where its heightmap runs across a
// cliff's recesses, the game shows the rock behind it from below, and so must the rays. Every other
// instance is seen from both sides (D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE).
#define GI_RAY_FLAGS RAY_FLAG_CULL_BACK_FACING_TRIANGLES

struct SceneHit
{
    bool hit;
    float distance;
    uint instance;  // Into g_Instances
    uint geometry;  // Within the instance's structure
    uint primitive; // Triangle within the geometry
    float2 barycentrics;
    bool frontFace; // Clockwise seen from the ray's origin (DXR's front)
};

// A ray leaving the surface a pixel shows: its cone starts as wide as the pixel there and spreads as
// `spread` (radians) does.
float2 PixelCone(float3 position, float spread)
{
    return float2(distance(position, g_Frame.eye.xyz) * g_Frame.rayCone.x, spread);
}

// The global illumination's rays' cone: as wide as a pixel where they start, spreading as a pixel's
// angle does (from the eye, a pixel's own cone).
float2 LightCone(float3 origin)
{
    return PixelCone(origin, g_Frame.rayCone.x);
}

// Instances whose mask shares a bit with `mask` take part. With `alphaTest`, cut-out geometry with its
// texture lets the ray through where it is clear, and cut-out geometry without blocks it as
// `untextured` says; the ray's cone (`cone`: its width at the origin and its growth a metre, metres)
// picks the texture's level of detail. Without, every triangle is opaque. Every ray tests alpha: the
// global illumination's rays once took every triangle as opaque, and a bank of grass, a heap of solid
// cards to them, went dark (2026-10-23). With `canMeetGlass`, glass stops it (GI_GEOMETRY_GLASS). With micromaps these rays take
// a micro-triangle that is part solid as what most of it is, and test no alpha on it: 3.4 to 3.8 ms off the jungle with those known
// to be all one or the other, past what a frame of them shows. A ray to the sun or a lamp past the near stretch tests alpha still,
// as there 1% of a frame moved by over 10% for 0.5 ms (SurfaceRayBlocked).
SceneHit TraceClosest(float3 origin, float3 direction, float minDistance, float maxDistance, uint mask, bool alphaTest, bool untextured, float2 cone,
    bool canMeetGlass = false)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = minDistance;
    ray.TMax = maxDistance;

    SceneHit result;
    if (alphaTest)
    {
        RayQuery<RAY_FLAG_NONE> query;
        query.TraceRayInline(g_Scene, GI_RAY_FLAGS | g_Frame.closestRayFlags.x, mask, ray);
        while (query.Proceed())
        {
            uint row = query.CandidateInstanceContributionToHitGroupIndex() + query.CandidateGeometryIndex();
            float footprint = cone.x + query.CandidateTriangleRayT() * cone.y;
            bool isGlassHit = canMeetGlass && (g_Geometries[row].flags & GI_GEOMETRY_GLASS) != 0;
            if (isGlassHit || CutOutBlocks(row, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), untextured, footprint))
                query.CommitNonOpaqueTriangleHit();
        }

        result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
        result.distance = result.hit ? query.CommittedRayT() : FLT_MAX;
        result.instance = result.hit ? query.CommittedInstanceIndex() : 0;
        result.geometry = result.hit ? query.CommittedGeometryIndex() : 0;
        result.primitive = result.hit ? query.CommittedPrimitiveIndex() : 0;
        result.barycentrics = result.hit ? query.CommittedTriangleBarycentrics() : 0.0f;
        result.frontFace = result.hit && query.CommittedTriangleFrontFace();
        return result;
    }

    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(g_Scene, GI_RAY_FLAGS, mask, ray);
    query.Proceed();

    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    result.distance = result.hit ? query.CommittedRayT() : FLT_MAX;
    result.instance = result.hit ? query.CommittedInstanceIndex() : 0;
    result.geometry = result.hit ? query.CommittedGeometryIndex() : 0;
    result.primitive = result.hit ? query.CommittedPrimitiveIndex() : 0;
    result.barycentrics = result.hit ? query.CommittedTriangleBarycentrics() : 0.0f;
    result.frontFace = result.hit && query.CommittedTriangleFrontFace();
    return result;
}

// A global illumination ray (LightCone); cut-out geometry without its texture lets it through.
SceneHit TraceClosest(float3 origin, float3 direction, float minDistance, float maxDistance)
{
    return TraceClosest(origin, direction, minDistance, maxDistance, GI_MASK_SCENE, true, false, LightCone(origin));
}

SceneHit TraceClosest(float3 origin, float3 direction, float maxDistance)
{
    return TraceClosest(origin, direction, 0.0f, maxDistance);
}

// A triangle's geometric normal, facing a ray along `direction`; against the ray for a degenerate one.
float3 FacingNormal(SceneTriangle t, float3 direction)
{
    float3 normal = cross(t.v1 - t.v0, t.v2 - t.v0);
    float lengthSquared = dot(normal, normal);
    normal = lengthSquared > 0.0f ? normal * rsqrt(lengthSquared) : -direction;
    return dot(normal, direction) > 0.0f ? -normal : normal;
}

// The geometric normal of a hit triangle, facing the ray.
float3 HitNormal(SceneHit hit, float3 direction)
{
    return FacingNormal(LoadTriangle(hit.instance, hit.geometry, hit.primitive), direction);
}

// Diagnostics: adds the lanes for which `counted` holds to a counter, one atomic a wave.
void CountDiagnostic(uint counter, bool counted)
{
    uint count = WaveActiveCountBits(counted);
    if (WaveIsFirstLane() && count) InterlockedAdd(g_Counters[counter], count);
}

// Diagnostics: the wave's largest channel into a GI_COUNTER_MAX_* counter, one atomic a wave. Light is
// never negative, so its bits order as its values do (a NaN as the largest of all).
void MaxDiagnostic(uint counter, float3 value)
{
    float largest = WaveActiveMax(max(max(value.r, value.g), value.b));
    if (WaveIsFirstLane() && largest > 0.0f) InterlockedMax(g_Counters[counter], asuint(largest));
}

// ---- Point and spot lights ----
// A point or spot light's light reaching `world`, without shadows, as the game's light shaders work it out
// (Lighting_*_NA, SSLighting2_*), in the sun colour's units, before the surface's cosine: its falloff, a
// spot's cone, and the game's volume for it; its direction and distance in `toLight` and `lightDistance`. A
// spot's mask (a flashlight's pattern) is left out.
float3 LocalLightReaching(GILocalLight light, float3 world, out float3 toLight, out float lightDistance)
{
    float3 offset = light.position.xyz - world;
    lightDistance = length(offset);
    toLight = offset / max(lightDistance, 1e-6f);
    if (lightDistance >= light.position.w) return 0.0f;

    // The game lights only within the light's volume: a fill light's box keeps its 200 m of reach to its room.
    const float margin = GI_LIGHT_VOLUME_MARGIN;
    if (light.volumeLow.w > 0.0f && (any(world < light.volumeLow.xyz - margin) || any(world > light.volumeHigh.xyz + margin))) return 0.0f;

    // A point's distance is at least its least, plus its added; a spot's is as it is.
    float d = max(lightDistance, light.falloff.z) + light.falloff.y;
    float d2 = d * d;
    float falloff = light.falloff.x > 0.0f ? (1.0f - min(d2 * d2 * d2 * d2 * d2 * d2 * d2 * d2, FLT_MAX) * light.color.w) / d2
                                           : 1.0f / d2 - d2 * light.color.w;
    falloff = max(min(falloff, light.shape.w), 0.0f);

    // A spot's cone: saturate((cosine - umbra) / penumbra) to its exponent; a point's "cone" is everywhere.
    float cut = 1.0f;
    if (light.axis.w >= -1.0f) cut = pow(saturate((dot(light.axis.xyz, -toLight) - light.axis.w) * light.shape.y), light.shape.z);

    return light.color.rgb * (falloff * cut);
}

// The frame's lights that can reach `world`, a bit each, in the order of g_Frame.localLights (light_bins.hpp).
uint4 LightsNear(float3 world)
{
    int3 bin = int3(floor((world - g_Frame.lightBinsOrigin.xyz) / GI_LIGHT_BIN_SIZE));
    bin = clamp(bin, int3(0, 0, 0), int3(GI_LIGHT_BINS_ACROSS, GI_LIGHT_BINS_HIGH, GI_LIGHT_BINS_ACROSS) - 1);

    return g_Frame.lightBinsX[bin.x] & g_Frame.lightBinsY[bin.y] & g_Frame.lightBinsZ[bin.z];
}

// The first of `lights`, as LightsNear gives them, from the light `from` on; GI_MAX_LOCAL_LIGHTS past the last. A walk from 0
// keeps the order of g_Frame.localLights.
uint NextLight(uint4 lights, uint from)
{
    uint bitsFrom = ~0u << (from % 32u);
    for (uint word = from / 32u; word < 4u; ++word)
    {
        uint bits = lights[word] & bitsFrom;
        if (bits != 0u) return word * 32u + firstbitlow(bits);

        bitsFrom = ~0u;
    }

    return GI_MAX_LOCAL_LIGHTS;
}

// Its diffuse light at a surface of `normal`.
float3 LocalLightAt(GILocalLight light, float3 world, float3 normal, out float3 toLight, out float lightDistance)
{
    float3 reaching = LocalLightReaching(light, world, toLight, lightDistance);
    float cosine = dot(normal, toLight);
    return cosine > 0.0f ? reaching * cosine : 0.0f;
}

// How far short of a light its shadow rays towards it (`toLight`, from the surface) end: the lamp around
// it; for a spot the game shadows with a shadow map, wherever that map starts along the ray, as nothing
// nearer the light is in it.
float ClearanceTowards(GILocalLight light, float3 toLight)
{
    float alongAxis = dot(light.axis.xyz, -toLight);
    float mapStart = light.shadow.x > 0.0f && alongAxis > 0.0f ? light.shadow.x / alongAxis : 0.0f;
    return max(light.shape.x, mapStart);
}

// ---- Copies of a surface ----
// The scene can hold a surface twice, a little apart: the game swaps levels of detail, and draws
// some meshes at another level of detail for the shadows. A ray leaving one copy would find the
// other just above it and see nothing but its underside. Rays from a surface skip what faces the
// same way within this distance above where they start; it grows with the distance from the camera,
// as the levels of detail coarsen.
float CopyTolerance(float3 position)
{
    return min(0.05f + 0.005f * distance(position, g_Frame.eye.xyz), 0.5f);
}

// A copy faces the way the surface does, so a ray leaving the surface meets the copy's back: on a model, DXR's front
// (StopsShadowRay). A surface facing the ray is something else: until 2026-11-06 any parallel one counted, and with the
// rays from a hit lifted 3 mm, not 3 cm, the bounce rays from a pipe 1 to 2 cm off a room's wall still took the wall for a
// copy of the pipe and went through it (docs/DEVELOPMENT.md, "Rays leaving a hit"). Cut-out geometry, which the game
// mostly draws two-sided, is a copy from either side; the terrain and the sea, culled from behind, never are.
bool IsCopyOfSurface(SceneHit hit, float3 normal, float3 direction, float tolerance)
{
    float height = hit.distance * dot(direction, normal);
    if (height < 0.0f || height >= tolerance) return false;
    if (abs(dot(HitNormal(hit, direction), normal)) <= 0.85f) return false;

    GIInstanceDesc desc = g_Instances[hit.instance];
    if (!(InstanceKind(desc) & GI_INSTANCE_MODEL)) return false;

    return hit.frontFace || (g_Geometries[FirstGeometry(desc) + hit.geometry].flags & GI_GEOMETRY_CUT_OUT) != 0;
}

// Whether a hit stops a ray towards a light as the game's shadow maps would have it. They are drawn with
// front faces culled (1,227 of 1,267 shadow map draws, 2026-10-26): an opaque surface casts from its back
// only, as the light sees it, so a ray towards the light meets it on its front. A window's one-sided card,
// its front outwards, let the searchlight into the hospital's corridor in the game and kept it out of the
// rays'. Cut-out geometry, which the game mostly draws two-sided, casts from both sides. The game's faces
// on its models are DXR's back faces (its projection mirrors x); the terrain is one-sided already. A
// mesh the game never draws into its shadow maps (a rigid model's main-view-only one) does not stop it
// at all: one in a stairwell's windows kept the moonlight off its wall (the lamp probe, 2026-10-26). A
// building's stand-in (GI_GEOMETRY_SUN_ONLY) stops the sun's rays alone: the game draws it into every sun
// cascade, and it stands in for main-view-only walls and rock that let the sun into a room and a cave
// (2026-10-27); a lamp's rays pass it, as it kept a room's lamp off a wall where nothing was drawn.
bool StopsShadowRay(uint instance, uint geometry, bool dxrFrontFace, bool isTowardsLamp)
{
    GIInstanceDesc desc = g_Instances[instance];
    if (!(InstanceKind(desc) & GI_INSTANCE_MODEL)) return true;

    uint flags = g_Geometries[FirstGeometry(desc) + geometry].flags;
    if (flags & GI_GEOMETRY_NO_SHADOW) return false;
    if (isTowardsLamp && (flags & GI_GEOMETRY_SUN_ONLY)) return false;

    return !dxrFrontFace || (flags & GI_GEOMETRY_CUT_OUT) != 0;
}

// What a ray leaving a surface goes towards, for what stops it (StopsRay).
static const uint RayTowardsSurface = 0u; // The camera's and the global illumination's rays
static const uint RayTowardsSun = 1u;
static const uint RayTowardsLamp = 2u;
static const uint RayAlongReflection = 3u; // Towards a surface too, and it meets glass (TraceReflectionRay)

// Rays towards a light take the game's shadow maps' faces (StopsShadowRay), unless the diagnostics ask
// for every surface from both sides, as until 2026-10-26 (shadowDiagnostics.x): they are then taken as
// rays towards a surface.
uint ShadowRayTowards(uint light)
{
    return g_Frame.shadowDiagnostics.x == 0u ? light : RayTowardsSurface;
}

// Whether a hit stops a ray: one towards a light as the game's shadow maps would have it
// (StopsShadowRay); any other ray passes what the game draws into its shadow maps alone (a stand-in,
// GI_GEOMETRY_SHADOW_ONLY), as the camera never sees it, and a self-lit mesh while those give no light
// (g_Frame.emission.x: without path tracing, or with its slider at 0): it would be a black hit.
bool IsShadowOnly(uint instance, uint geometry)
{
    GIInstanceDesc desc = g_Instances[instance];
    if (!(InstanceKind(desc) & GI_INSTANCE_MODEL)) return false;

    return (g_Geometries[FirstGeometry(desc) + geometry].flags & GI_GEOMETRY_SHADOW_ONLY) != 0;
}

bool IsDarkSelfLit(uint instance, uint geometry)
{
    GIInstanceDesc desc = g_Instances[instance];
    if (g_Frame.emission.x > 0.0f || !(InstanceKind(desc) & GI_INSTANCE_MODEL)) return false;

    return (g_Geometries[FirstGeometry(desc) + geometry].flags & GI_GEOMETRY_EMISSIVE) != 0;
}

bool StopsRay(uint instance, uint geometry, bool dxrFrontFace, uint towards)
{
    if (towards == RayTowardsSurface || towards == RayAlongReflection) return !IsShadowOnly(instance, geometry) && !IsDarkSelfLit(instance, geometry);

    return StopsShadowRay(instance, geometry, dxrFrontFace, towards == RayTowardsLamp);
}

// Hair lets light through. It is a cloud of thin strands, and renderers shadow with it by how much light
// gets past the strands, not by whether any is in the way (Lokovic and Veach, "Deep Shadow Maps", 2000;
// Yuksel and Keyser, "Deep Opacity Maps", 2008, as Frostbite's and Unreal's strand hair do). The game
// draws its hair cards into none of its shadow maps; taken as solid, they cast hard, fully dark strands.
// A ray towards a light keeps this share of its light past each layer of hair it crosses (GI_GEOMETRY_HAIR):
// a single layer casts a light shadow, a thick head of hair a dark one.
static const float HairTransmittance = 0.5f;

bool IsHair(uint row)
{
    return (g_Geometries[row].flags & GI_GEOMETRY_HAIR) != 0;
}

// A ray towards a light passing a layer of hair `distance` along it.
void PassHair(float distance, inout float transmittance, inout float nearestHair)
{
    transmittance *= HairTransmittance;
    nearestHair = min(nearestHair, distance);
}

// Whether a ray towards a light (not one towards a surface or along a reflection) is dimmed by the panes it crosses
// (PaneTransmittance). Glass is non-opaque and never committed, so each candidate pane is met once
// (NO_DUPLICATE_ANYHIT_INVOCATION) and the ray goes on.
bool DimsThroughGlass(uint row, uint towards)
{
    return (towards == RayTowardsSun || towards == RayTowardsLamp) && (g_Geometries[row].flags & GI_GEOMETRY_GLASS) != 0;
}

// Whether a pane a ray meets is the one it left. A ray towards the sun or a lamp starts a couple of centimetres off a
// surface, so from a pane it crosses that pane at once: dimmed by its own cover, every pane cast its dirt's shadow onto
// itself, and the mask's frame and streaks read as a shadow of the window on the glass, from the side the eye is on.
// A pane that the ray goes through lies further from the plane it started on.
bool IsOwnPane(float3 origin, float3 direction, float distance, float3 normal)
{
    const float OwnPlane = 0.05f;
    return distance * abs(dot(direction, normal)) < OwnPlane;
}

// How wide a patch of a pane a ray towards a light reads its mask over. One fixed width, whatever the camera and the
// distance: read at the camera's pixel, the mask's detail came and went with the camera, and each frame's point on the
// sun's disc crossed a sharper level at another texel, so a pane's dirt's shadow appeared in patches and boiled near.
// The width is the sun's disc at a room's distance (2 x tan of its radius x 2 m), soft enough to hold still.
static const float PaneShadowBlur = 0.02f;

// How far apart two panes a ray crosses must lie to be two sheets of glass. Many panes are a quad for each side, the
// same sheet twice: a window at the Afghan huts (84a2aa9c6ae74858 mesh 1, 4 triangles a sash) dimmed the sun on the floor
// to 0.54 where its one sheet lets through 0.73.
static const float SamePane = 0.01f;

// A ray towards a light, from `origin` on a surface of `normal`, crossing a pane `distance` along it (`objectDirection`
// in the pane's object space): past the pane it left (IsOwnPane) and the other side of the one it last crossed
// (`countedPane`, its distance, negative for none), it keeps what the pane lets through, and the pane takes its light as
// hair does (PassHair).
void PassPane(uint row, uint primitive, float2 barycentrics, float distance, float3 objectDirection, float3 origin, float3 direction,
    float3 normal, inout float transmittance, inout float nearestHair, inout float countedPane)
{
    if (IsOwnPane(origin, direction, distance, normal)) return;

    GIGeometry geometry = g_Geometries[row];
    uint3 indices = TriangleIndices(geometry, primitive);
    float cosine = PaneCosine(geometry, indices, objectDirection);
    if (countedPane >= 0.0f && abs(distance - countedPane) * cosine < SamePane) return;

    transmittance *= PaneTransmittance(geometry, indices, barycentrics, PaneShadowBlur, cosine);
    nearestHair = min(nearestHair, distance);
    countedPane = distance;
}

// The closest hit of a ray leaving a surface of `normal` from `origin`, past copies of the surface, and
// in `copies` how many it skipped; `alphaTest` as TraceClosest. Past what does not stop it too (StopsRay:
// by what it goes `towards`). With `throughHair`, past hair too, which takes its share of
// the ray's `transmittance` and leaves the nearest layer's distance in `nearestHair` (PassHair).
SceneHit TraceFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance, uint mask, bool alphaTest, float2 cone,
    uint towards, bool throughHair, inout float transmittance, inout float nearestHair, out uint copies)
{
    copies = 0;
    float start = 0.0f;
    SceneHit hit;
    for (uint attempt = 0; attempt < 6; ++attempt)
    {
        // A ray's TMin must not pass its TMax: a hit passed right at the ray's end leaves nothing to look for.
        if (start >= maxDistance) break;

        hit = TraceClosest(origin, direction, start, maxDistance, mask, alphaTest, false, cone, towards == RayAlongReflection);
        bool copy = hit.hit && IsCopyOfSurface(hit, normal, direction, tolerance);
        CountDiagnostic(GI_COUNTER_COPIES, copy);
        bool passed = copy || (hit.hit && !StopsRay(hit.instance, hit.geometry, hit.frontFace, towards));
        bool hair = !passed && throughHair && hit.hit && IsHair(FirstGeometry(g_Instances[hit.instance]) + hit.geometry);
        if (!passed && !hair) return hit;

        if (hair) PassHair(hit.distance, transmittance, nearestHair);
        if (copy) ++copies;
        start = hit.distance * 1.0001f + 1e-3f;
    }

    hit.hit = false;
    hit.distance = FLT_MAX;
    return hit;
}

SceneHit TraceFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance, uint mask, bool alphaTest, float2 cone,
    uint towards, out uint copies)
{
    float transmittance = 1.0f, nearestHair = FLT_MAX;
    return TraceFromSurface(origin, normal, tolerance, direction, maxDistance, mask, alphaTest, cone, towards, false, transmittance, nearestHair, copies);
}

SceneHit TraceFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance, uint mask, bool alphaTest, float2 cone,
    out uint copies)
{
    return TraceFromSurface(origin, normal, tolerance, direction, maxDistance, mask, alphaTest, cone, RayTowardsSurface, copies);
}

SceneHit TraceFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance, uint mask, bool alphaTest, float2 cone)
{
    uint copies;
    return TraceFromSurface(origin, normal, tolerance, direction, maxDistance, mask, alphaTest, cone, RayTowardsSurface, copies);
}

// A global illumination ray leaving a surface (LightCone).
SceneHit TraceFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance)
{
    return TraceFromSurface(origin, normal, tolerance, direction, maxDistance, GI_MASK_SCENE, true, LightCone(origin));
}

// How far along a ray leaving a surface copies of the surface can be: where it rises `tolerance` above it.
float NearEnd(float3 normal, float tolerance, float3 direction, float maxDistance)
{
    return min(tolerance / max(dot(direction, normal), 0.1f), maxDistance);
}

// Whether a ray leaving a surface is stopped before `maxDistance`; in `transmittance` the share of its
// light the hair it passes lets through (PassHair: hair never stops it), and in `blocker` how far along
// the nearest thing that takes its light is, what stops it or hair (FLT_MAX where nothing does). The
// stretch where copies of the surface can be (NearEnd) skips them and takes the closest hit; on from
// there the first hit found ends it. Cut-out geometry with the game's alpha blocks it where its alpha
// does. Cut-out geometry without its texture lets it through; with `chance`, it blocks it with chance
// `blocking`, a random number drawn a candidate, so the denoiser averages a partial shadow. Only what
// stops it blocks it (StopsRay: by what it goes `towards`).
bool SurfaceRayBlocked(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance, float2 cone, bool chance, float blocking,
    uint towards, inout Random random, out float blocker, out float transmittance)
{
    transmittance = 1.0f;
    float nearestHair = FLT_MAX;
    float nearEnd = NearEnd(normal, tolerance, direction, maxDistance);
    // Rays towards lights skip what they would pass anyway, whole (GI_MASK_VIEW_ONLY).
    uint mask = (towards == RayTowardsSun || towards == RayTowardsLamp) ? GI_MASK_CASTS : GI_MASK_SCENE;
    uint copies;
    SceneHit nearHit = TraceFromSurface(origin, normal, tolerance, direction, nearEnd, mask, true, cone, towards, true, transmittance,
        nearestHair, copies);
    blocker = min(nearHit.distance, nearestHair);
    if (nearHit.hit) return true;
    if (nearEnd >= maxDistance) return false;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = nearEnd;
    ray.TMax = maxDistance;

    // What the near stretch let through, for the second look below.
    float nearTransmittance = transmittance, nearHair = nearestHair;
    float countedPane = -1.0f;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;
    query.TraceRayInline(g_Scene, GI_RAY_FLAGS, mask, ray);
    // Only cut-out geometry is non-opaque, so only it comes here. What does not stop it (a tree fern's fronds, which only the
    // camera's and the global illumination's rays meet) is passed here, not left for the second look.
    while (query.Proceed())
    {
        if (!StopsRay(query.CandidateInstanceIndex(), query.CandidateGeometryIndex(), query.CandidateTriangleFrontFace(), towards)) continue;

        uint row = query.CandidateInstanceContributionToHitGroupIndex() + query.CandidateGeometryIndex();
        float footprint = cone.x + query.CandidateTriangleRayT() * cone.y;
        if (DimsThroughGlass(row, towards))
        {
            if (StopsRay(query.CandidateInstanceIndex(), query.CandidateGeometryIndex(), query.CandidateTriangleFrontFace(), towards))
                PassPane(row, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), query.CandidateTriangleRayT(),
                    query.CandidateObjectRayDirection(), origin, direction, normal, transmittance, nearestHair, countedPane);
            continue;
        }

        bool untextured = false;
        if (chance) untextured = random.rand() < blocking;
        if (!CutOutBlocks(row, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), untextured, footprint))
            continue;

        if (IsHair(row)) PassHair(query.CandidateTriangleRayT(), transmittance, nearestHair);
        else query.CommitNonOpaqueTriangleHit();
    }

    bool blocked = query.CommittedStatus() != COMMITTED_NOTHING;
    blocker = min(blocked ? query.CommittedRayT() : FLT_MAX, nearestHair);

    // Any hit that stops it settles it. One that does not (a surface met from its back, a stand-in) leaves
    // it open: the ray again, every triangle looked at.
    if (!blocked || StopsRay(query.CommittedInstanceIndex(), query.CommittedGeometryIndex(), query.CommittedTriangleFrontFace(), towards))
        return blocked;

    transmittance = nearTransmittance;
    nearestHair = nearHair;
    countedPane = -1.0f;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> every;
    every.TraceRayInline(g_Scene, GI_RAY_FLAGS, mask, ray);
    while (every.Proceed())
    {
        uint instance = every.CandidateInstanceIndex(), geometry = every.CandidateGeometryIndex();
        if (!StopsRay(instance, geometry, every.CandidateTriangleFrontFace(), towards)) continue;

        uint row = every.CandidateInstanceContributionToHitGroupIndex() + geometry;
        if (DimsThroughGlass(row, towards))
        {
            PassPane(row, every.CandidatePrimitiveIndex(), every.CandidateTriangleBarycentrics(), every.CandidateTriangleRayT(),
                every.CandidateObjectRayDirection(), origin, direction, normal, transmittance, nearestHair, countedPane);
            continue;
        }

        bool untextured = false;
        if (chance) untextured = random.rand() < blocking;
        float footprint = cone.x + every.CandidateTriangleRayT() * cone.y;
        bool cutOut = (g_Geometries[row].flags & GI_GEOMETRY_CUT_OUT) != 0;
        if (cutOut && !CutOutBlocks(row, every.CandidatePrimitiveIndex(), every.CandidateTriangleBarycentrics(), untextured, footprint))
            continue;

        if (IsHair(row)) PassHair(every.CandidateTriangleRayT(), transmittance, nearestHair);
        else every.CommitNonOpaqueTriangleHit();
    }

    blocked = every.CommittedStatus() != COMMITTED_NOTHING;
    blocker = min(blocked ? every.CommittedRayT() : FLT_MAX, nearestHair);
    return blocked;
}

// The share of its light a global illumination ray (LightCone) leaving a surface gets away with: 0 where
// something stops it, what the hair it passes lets through otherwise; cut-out geometry without its texture
// lets it through. Stopped by what it goes `towards` (SurfaceRayBlocked).
float VisibilityFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance, uint towards)
{
    Random unused = (Random)0;
    float blocker, transmittance;
    bool blocked = SurfaceRayBlocked(origin, normal, tolerance, direction, maxDistance, LightCone(origin), false, 0.0f, towards, unused, blocker,
        transmittance);
    return blocked ? 0.0f : transmittance;
}

// ---- The game's terrain ----
// The game's virtual heightmap, as its terrain vertex shaders read it (NewTerrain_*: t0 the page
// table, t1 the height atlas), for the rays: rings of cells around the camera, finer near
// (TerrainGrid in gi_scene.hlsl).
Texture2D<float4> g_TerrainPages : register(t14);
Texture2D<float> g_TerrainHeights : register(t15);

float2 TerrainUv(float2 xz)
{
    return min((xz - g_Frame.terrainOffset.xz) / g_Frame.terrainScale.xz, 0.999756f);
}

// The atlas tile (xy) and level of detail (z) resident for virtual heightmap coordinates.
float3 TerrainTile(float2 virtualUv)
{
    return floor(g_TerrainPages.SampleLevel(g_PointSampler, virtualUv, 0.0f).xyz * 255.4f);
}

// The height at virtual heightmap coordinates, before the terrain's scale: the tile's texel in the atlas.
float TerrainHeight(float2 virtualUv)
{
    float3 tile = TerrainTile(virtualUv);
    float2 tileUv = frac(virtualUv * g_Frame.terrainUvConverter.xy * exp2(-tile.z)) + tile.xy;
    float2 physicalUv = tileUv * g_Frame.terrainUvConverter.zw + g_Frame.terrainTexelCorrect.xy;
    return g_TerrainHeights.SampleLevel(g_LinearSampler, physicalUv, 0.0f);
}

float3 TerrainPoint(float2 xz)
{
    return float3(xz.x, TerrainHeight(TerrainUv(xz)) * g_Frame.terrainScale.y + g_Frame.terrainOffset.y, xz.y);
}

// The game's terrain vertex at virtual heightmap coordinates, as its terrain vertex shader (NewTerrain_Depth) places
// it: the coordinates clamped as it clamps them, then (u, height, v) x its draw's scale + offset (cb5 c10, c11).
float3 TerrainVertex(float2 virtualUv, float3 scale, float3 offset)
{
    float2 uv = min(virtualUv, 0.999756f);
    return float3(uv.x, TerrainHeight(uv), uv.y) * scale + offset;
}

// The scene's terrain over `xz`: its innermost ring's two triangles over the cell, split as TerrainGrid splits them, not
// the heightmap between the cell's corners. Minus infinity outside that ring, or without terrain.
float SceneTerrainHeight(float2 xz)
{
    float4 ring = g_Frame.terrainRings[0];
    float2 cells = (xz - ring.xy) / ring.z;
    if (g_Frame.terrainScale.w == 0.0f || any(cells < 0.0f) || any(cells >= ring.w)) return -FLT_MAX;

    float2 corner = ring.xy + floor(cells) * ring.z;
    float2 f = frac(cells);
    float h10 = TerrainPoint(corner + float2(ring.z, 0.0f)).y, h01 = TerrainPoint(corner + float2(0.0f, ring.z)).y;
    if (f.x + f.y <= 1.0f)
    {
        float h00 = TerrainPoint(corner).y;
        return h00 + f.x * (h10 - h00) + f.y * (h01 - h00);
    }

    float h11 = TerrainPoint(corner + ring.z).y;

    return h11 + (1.0f - f.x) * (h01 - h11) + (1.0f - f.y) * (h10 - h11);
}

// ---- Where a pixel's rays leave its surface ----
// The scene's own surface where a ray from the eye through the pixel meets it near the depth's point: its
// exact point and its triangle's normal, from which a ray leaves past that very surface (LiftOffTriangle),
// so nothing near it needs skipping.
// Where the game drew something the scene lacks or holds at another level of detail, the depth's point
// and the G-buffer's normal, with copies of the surface skipped ("Copies of a surface"). Until 2026-10-24
// every ray left that way: the skip took a character pressed to a wall for a copy of the wall, and
// light went through him, ragged along the wall's bumpy normals.
struct PixelSurface
{
    float3 position;
    float3 normal; // Towards the eye: the triangle's on the scene's surface, else the G-buffer's
    bool isExact;  // On the scene's own surface
    SceneHit hit;  // Diagnostics (the probe): the eye's ray's hit on it
};

// Where the add-on's sea drew (the pack's albedo alpha, ray_tracing.cpp).
bool IsSeaPixel(uint2 pixel)
{
    return g_Albedo.Load(int3(pixel, 0)).a < 0.5f;
}

// What the camera never sees as itself: a shadow-only stand-in (a tarp's slab, 16 cm before its sheet), or the sea's
// plane in place of the waves. Taken as the surface, the plane lit a hull's wall below the plane's level, which the
// camera's ray crosses the plane just before, as open water: a pale band at the waterline (2026-11-05).
bool IsStandIn(SceneHit hit)
{
    return IsShadowOnly(hit.instance, hit.geometry) || (InstanceKind(g_Instances[hit.instance]) & GI_INSTANCE_SEA) != 0;
}

// The terrain's top, where the game drew a surface facing the other way. The terrain stands up to 3 cm off the ground
// the game draws (TerrainGap), above it there, so Snake's arm lying there dips under it: the search took the terrain 4 cm before the arm's underside
// as its surface, and the rays left upwards from it into the sky, a pale band along the
// arm's edge at 2.4x the arm's light (a debug case, 2026-11-05). The camera's ray sees only the terrain's top.
bool IsTerrainFacingAway(SceneHit hit, float3 direction, float3 gbufferNormal)
{
    if ((InstanceKind(g_Instances[hit.instance]) & GI_INSTANCE_TERRAIN) == 0) return false;

    return dot(FacingNormal(LoadTriangle(hit.instance, hit.geometry, hit.primitive), direction), gbufferNormal) < 0.0f;
}

// How far the terrain may stand off the ground the game draws, along the ground's normal, and still be taken as it. The game
// draws its terrain from patches whose grid coarsens with distance (32 down to 4 cells a patch), the scene's rings from 1 m
// cells, so the two part by up to 3 cm on a slope (a debug case, 2026-11-05); the scene takes the game's own patches where it
// drew them (TerrainPatches), and the rings stand only where it did not.
static const float TerrainGap = 0.03f;

// The terrain under a ground pixel the search for its surface missed. Along a camera ray grazing the ground, a 3 cm gap is
// 10 cm, past that search's reach: the pixels by a boot resting on a slope left from the depth's point, lifted 2.3 cm into
// the boot, and their ground went black, 7x darker than the game's (50 of 64 rays inside the boot). The terrain within
// TerrainGap of the depth's point along the terrain's own normal is taken, however far along the ray: the G-buffer's normal
// is the bumps', and read 3.6 cm for a 1.8 cm gap there. Only where that normal is the ground's, within 60 degrees of the
// terrain's: a boot's side by the ground is not the terrain.
SceneHit TerrainUnderPixel(float3 position, float3 direction, float surfaceDistance, float3 gbufferNormal)
{
    // Nowhere near the terrain, as most pixels the search misses are (a roof the scene leaves out, leaves): no ray. Traced for
    // every one, it cost 0.3 ms at 4K on a tin roof. Twice the gap upright is that gap along a slope's normal up to 60 degrees.
    SceneHit hit = (SceneHit)0;
    if (abs(SceneTerrainHeight(position.xz) - position.y) > 2.0f * TerrainGap) return hit;

    const float reach = TerrainGap / 0.05f; // A camera ray down to 3 degrees off the ground
    hit = TraceClosest(g_Frame.eye.xyz, direction, max(surfaceDistance - reach, 0.0f), surfaceDistance + reach, GI_MASK_TERRAIN, false, false,
        float2(0.0f, g_Frame.rayCone.x));
    if (!hit.hit) return hit;

    float3 terrainNormal = FacingNormal(LoadTriangle(hit.instance, hit.geometry, hit.primitive), direction);
    float gap = abs(hit.distance - surfaceDistance) * abs(dot(direction, terrainNormal));
    hit.hit = gap <= TerrainGap && dot(terrainNormal, gbufferNormal) >= 0.5f;

    return hit;
}

PixelSurface PixelSurfaceAt(uint2 pixel, float3 position, float3 gbufferNormal)
{
    PixelSurface surface;
    surface.position = position;
    surface.normal = gbufferNormal;
    surface.isExact = false;
    surface.hit = (SceneHit)0;

    // The scene holds only the sea's flat plane, not its waves: the search took the plane wherever a wave came within its
    // reach and the wave's own point elsewhere, so the sun's shadow on the water jumped between the two as the waves
    // moved, and it took a hull's wall behind the water by it, the water there lit as that wall (a debug case at
    // Mother Base, 2026-11-05). For the same reason the plane is no other pixel's surface (IsStandIn).
    if (IsSeaPixel(pixel)) return surface;

    // Only the stretch around the depth's point: the game hides what comes between the camera and the player
    // (a bush the camera backs into) and the scene keeps it. Searched from the eye, the ray stopped on its
    // leaves and the pixels behind them left from the depth's point, inside the coarser rock behind for some:
    // the bush's leaves showed as shadows on what lay behind (the shadow probe, 2026-10-24: the camera's ray
    // met leaves 0.1 to 1.4 m away, the game's surface 2.4 to 5.9 m).
    float3 toSurface = position - g_Frame.eye.xyz;
    float surfaceDistance = length(toSurface);
    float3 direction = toSurface / max(surfaceDistance, 1e-6f);
    float tolerance = 0.03f + 0.02f * surfaceDistance;
    float searchFrom = max(surfaceDistance - tolerance, 0.0f);
    SceneHit hit;
    bool isPassedOver = false;
    // A stand-in, or the terrain where the game drew a surface facing the other way, is not the surface: what the game drew
    // may lie behind it.
    for (uint attempt = 0; attempt < 4; ++attempt)
    {
        hit = TraceClosest(g_Frame.eye.xyz, direction, searchFrom, surfaceDistance + tolerance, GI_MASK_SCENE, true, false, float2(0.0f, g_Frame.rayCone.x));
        isPassedOver = hit.hit && (IsStandIn(hit) || IsTerrainFacingAway(hit, direction, gbufferNormal));
        if (!isPassedOver) break;

        // A triangle's hit lies strictly past a ray's TMin, so this one drops out and what lies right behind it stays.
        searchFrom = hit.distance;
    }

    bool isFound = hit.hit && abs(hit.distance - surfaceDistance) <= tolerance && !isPassedOver;
    if (!isFound) hit = TerrainUnderPixel(position, direction, surfaceDistance, gbufferNormal);
    if (!hit.hit) return surface;

    SceneTriangle t = LoadTriangle(hit.instance, hit.geometry, hit.primitive);
    float3 normal = cross(t.v1 - t.v0, t.v2 - t.v0);
    float lengthSquared = dot(normal, normal);
    if (!(lengthSquared > 0.0f)) return surface;

    normal *= rsqrt(lengthSquared);
    surface.normal = dot(normal, direction) > 0.0f ? -normal : normal;
    surface.position = TrianglePoint(t, hit.barycentrics);
    surface.isExact = true;
    surface.hit = hit;

    return surface;
}

// A ray's start off a point on the scene's own surface, a pixel's or a hit's: 16 float steps of the point's largest
// coordinate along the normal, about 2 mm at 1,200 m from the map's origin, where the game's maps reach.
// The point is worked out from the triangle's corners, and the ray is traced in the instance's own space,
// each a few steps of the largest coordinate off in any direction: a character's transform turns the
// error of a large coordinate into the others. Wächter and Binder's offset (Ray Tracing Gems, chapter 6: 256 steps
// of each coordinate on its own) lifted the start about 3 cm in the field, more than the folds of a
// character's clothes, so it sat inside the next fold and his chest was shadowed in strips; and next to
// nothing in the helicopter, whose floor lies near the map's origin across but 1,200 m up, so a face
// turned sideways hit its own triangle in every other frame (the shadow probe, 2026-10-24). Rays from hits kept
// it until 2026-11-06, and those from a pipe 1 to 2 cm off a room's wall left from inside the wall (docs/DEVELOPMENT.md,
// "Rays leaving a hit"). Coordinates under 128 m count as 128: a large model's own coordinates reach that far.
float3 LiftOffTriangle(float3 position, float3 normal)
{
    const float steps = 16.0f;
    const float step = 1.0f / 8388608.0f; // A float step, relative

    float largest = max(max(abs(position.x), abs(position.y)), max(abs(position.z), 128.0f));
    return position + normal * (steps * step * largest);
}

// A pixel's ray leaves on the side `side` points to: off the triangle, past that very surface, or off the depth's point
// with copies of the surface skipped, where the scene has no surface there. Until 2026-11-02 the global illumination's and
// the opaque reflections' always left the depth's point, 4 cm along the bumpy normal at 10 m, which took them through the
// next wall in a room's corner: a ray from inside the wall reached the sun past the backs of its faces, and the corner lit
// up in bright seams (a debug case).
void PixelRayStart(PixelSurface surface, float3 side, out float3 origin, out float tolerance)
{
    origin = surface.isExact ? LiftOffTriangle(surface.position, side) : OffsetFromScreenSurface(surface.position, side);
    tolerance = surface.isExact ? 0.0f : CopyTolerance(surface.position);
}

// A bumpy normal tilts some of a pixel's rays under the triangle they leave: mirrored back above it. Left there, the
// reflections' showed as white veins along the bumps of a dark room's wall (a debug case, 2026-11-02).
float3 MirrorAboveSurface(float3 direction, float3 surfaceNormal)
{
    float alongNormal = dot(direction, surfaceNormal);
    return alongNormal < 0.0f ? direction - 2.0f * alongNormal * surfaceNormal : direction;
}

// The sun's direction, spread over its disc: a uniform point on a disc of the angular radius's tangent
// (the Sun size setting) around it.
float3 SunSample(float2 samples)
{
    float3 toSun = g_Frame.sunDirection.xyz;
    return normalize(toSun + DiscOffset(toSun, g_Frame.shadowParams.x, samples));
}

// How deep a pixel's model or character lies under the scene's terrain, as something lying on the ground sinks into it: the
// terrain stands up to 3 cm off the ground the game draws (TerrainGap), above it in places, so a prone body's underside dips
// under it (a debug case, 2026-11-05). 0 above the terrain, and deeper than a quarter metre, where it is a cliff's recess the heightmap runs over, not the
// ground under it. 0 too on the terrain itself, and at the depth's point, which may be the game's own ground under the terrain.
float SunkUnderTerrain(PixelSurface surface)
{
    if (!surface.isExact || (InstanceKind(g_Instances[surface.hit.instance]) & GI_INSTANCE_TERRAIN) != 0) return 0.0f;

    float depth = SceneTerrainHeight(surface.position.xz) - surface.position.y;

    return depth > 0.0f && depth < 0.25f ? depth : 0.0f;
}

#endif
