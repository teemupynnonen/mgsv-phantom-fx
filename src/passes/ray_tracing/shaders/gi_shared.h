// The global illumination's constants and records, shared by gi_renderer.cpp and the HLSL kernels
// in this folder. Every member is 16 bytes wide or packed into 16-byte groups, so HLSL's constant
// buffer packing and C++ agree.
#ifndef GI_SHARED_H
#define GI_SHARED_H

#ifdef __cplusplus
#include <cstdint>

namespace phantom_fx::gi
{
using uint = std::uint32_t;
struct uint2 { uint x, y; };
struct uint4 { uint x, y, z, w; };
struct float4 { float x, y, z, w; };
struct float4x4 { float m[16]; }; // Rows, applied as dot(row, v), as the engine's matrices are
#define GI_ROW_MAJOR
#else
#define GI_ROW_MAJOR row_major
#endif

// What an instance is: the low 8 bits of its InstanceID hold these.
#define GI_INSTANCE_MODEL     1u // One of the game's models (engine_scene.hpp), in object space under its transform
#define GI_INSTANCE_TERRAIN   2u // A ring of the game's terrain, made from its heightmap (TerrainGrid)
#define GI_INSTANCE_CHARACTER 4u // With GI_INSTANCE_MODEL: one of the game's skinned models, posed every frame (Skin)
#define GI_INSTANCE_KINDS     0xFFu

// What a geometry is (GIGeometry::flags).
#define GI_GEOMETRY_CUT_OUT       1u // Its texture cuts holes in it (leaves, nets, a pylon's lattice): non-opaque in its structure
#define GI_GEOMETRY_UV            2u // Its vertices' fourth floats hold their texture coordinates, two halves
#define GI_GEOMETRY_ALPHA_TEXTURE 4u // A cut-out geometry with its texture: the rays test its alpha (GIGeometry::alphaThreshold)
#define GI_GEOMETRY_MASK          8u // Its texture is a shadow-only mesh's mask: its alpha cuts, its colour is not the mesh's
#define GI_GEOMETRY_LEAF          16u // Leaves (the object brush's bushes and grass): thin, they let light through (HitDirectLight)
#define GI_GEOMETRY_NO_SHADOW     32u // The game never casts with it (a rigid model's main-view-only mesh): rays towards lights pass it (StopsShadowRay)
#define GI_GEOMETRY_HAIR          64u // A character's see-through mesh the game never casts with (its hair): rays towards lights pass it in part (PassHair)
#define GI_GEOMETRY_SHADOW_ONLY   128u // The game draws it into its shadow maps only (a tarp's stand-in): only rays towards lights meet it (StopsRay)
#define GI_GEOMETRY_SUN_ONLY      256u // With GI_GEOMETRY_SHADOW_ONLY (a building's stand-in): of the rays towards lights, only the sun's meet it (StopsShadowRay)
#define GI_GEOMETRY_TWO_SIDED     512u // The game casts with both its faces (a cut-out mesh it draws unculled, leaves, hair): rays towards lights meet either (StopsShadowRay)

// The game's textures the scene holds at most (g_SceneTextures, gi_textures.hpp).
#define GI_MAX_TEXTURES 4096u

// GIGeometry::indexOffset of a geometry without indices: three vertices a triangle.
#define GI_NO_INDICES 0xFFFFFFFFu

// A hit the global illumination keeps (gi_light.hlsl) packs its instance into the low bits of a uint
// and its geometry within the instance's structure above: 2^20 instances, 4096 geometries a structure.
#define GI_HIT_INSTANCE_BITS 20u

// The terrain's rings (TerrainGrid): so many, each so many cells across.
#define GI_TERRAIN_RINGS 4u
#define GI_TERRAIN_CELLS 256u

// The instances' mask: every ray takes them all.
#define GI_MASK_SCENE 1u

// The shadow probe's global illumination rays, one a thread of its group.
#define GI_PROBE_LIGHT_RAYS 64u

// Counters in the counter buffer.
#define GI_COUNTER_PACKED_TILES   0u // Two, by GIFrameConstants.hashMisc.y
#define GI_COUNTER_UPDATE_TILES   2u
#define GI_COUNTER_VISIBILITY0    3u // Second hits, the bounce rays', whose cells the light reads
#define GI_COUNTER_VISIBILITY1    4u // Third hits, from the second hits' cells' multibounce rays
#define GI_COUNTER_LIGHTS         5u // Hits whose light PopulateCells works out
// Diagnostics, read back with the output's statistics.
#define GI_COUNTER_HITS           6u // The light's rays that hit something
#define GI_COUNTER_NEAR_HITS      7u // The light's rays that hit something within 25 cm
#define GI_COUNTER_COPIES         8u // Hits skipped as copies of the surface a ray left
#define GI_COUNTER_FULL_BUCKETS   9u // Bounce hits the hash grid had no room for
#define GI_COUNTER_BOUNCE_HITS    10u // Bounce rays that hit something (the rest reach the sky)
#define GI_COUNTER_FOLIAGE        11u // Light pixels on the game's foliage (PrepareGuides)
#define GI_COUNTER_FOLIAGE_MEMORY_FULL 12u // Foliage light pixels the foliage memory had no room for (ResolveLight)
// The frame's largest value (asuint of a float, its largest channel) at each step of the light
// (MaxDiagnostic in gi_light.hlsl), to find where light grows past what the sun and sky give.
#define GI_COUNTER_MAX_ALBEDO     16u // A hit's albedo
#define GI_COUNTER_MAX_DIRECT     17u // A hit's direct light (TraceLight, PopulateCells)
#define GI_COUNTER_MAX_CELL       18u // A cell's direct light, after its update (UpdateTiles)
#define GI_COUNTER_MAX_INDIRECT   19u // A cell's indirect light, after its update
#define GI_COUNTER_MAX_BOUNCE     20u // A third hit's light into its second hit's cell (UpdateMultibounceCells)
#define GI_COUNTER_MAX_TRACED     21u // A ray's light, before the denoiser (ResolveLight)
#define GI_COUNTER_MAX_DENOISED   22u // A light pixel's denoised light (CompositeLight)
#define GI_COUNTER_MAX_OUTPUT     23u // A pixel's light at the render size, before the exposure (CompositeLight)
#define GI_COUNTER_MAXIMA_FIRST   16u
#define GI_COUNTER_MAXIMA         8u
#define GI_COUNTER_COUNT          32u
#define GI_COUNTER_NONE           0xFFFFFFFFu

// The light's denoiser (gi_denoiser.cpp) takes pixels nearer than its range; the sky's view depth lies beyond.
#define GI_LIGHT_DENOISING_RANGE 1e5f
#define GI_LIGHT_SKY_VIEW_Z 1e6f
// The denoiser's material for the game's foliage (the other pixels' is 0), which it takes as strands
// (gi_denoiser.cpp).
#define GI_LIGHT_FOLIAGE_MATERIAL 1.0f

// Slots of the indirect dispatch argument buffer.
#define GI_DISPATCH_SLOTS 16u

// The foliage memory (gi_light.hlsl, "Foliage's light by world position"): slots, in buckets of so many.
#define GI_FOLIAGE_MEMORY_SLOTS  (1u << 19)
#define GI_FOLIAGE_MEMORY_BUCKET 8u

// The point and spot lights a frame's shadows can hold (TraceLocalLightShadows in gi_shadow.hlsl). A base
// at night drew about 100.
#define GI_MAX_LOCAL_LIGHTS 128u

// A point or spot light as its shadow rays and the global illumination's hits need it: a point in the
// world whose light falls off as the game's light shaders have it (LocalLightAt in gi_light.hlsl), cut
// off past a range, and a spot's outside a cone.
struct GILocalLight
{
    float4 position; // xyz: the light, world space; w: how far its light reaches
    float4 axis;     // xyz: the way a spot points, world space; w: the cosine of its cone's outer edge (cPSLight c7.x), below -1 for a point
    float4 shape;    // x: how far short of the light its rays end, the lamp around it; y: the cone's 1 / penumbra (c7.y); z: its exponent (c7.z);
                     // w: the most a spot's falloff gives (cPSMaterial c7.w)
    float4 color;    // rgb: its colour (c4) times the global illumination's light scale; w: its falloff's factor (c6.z, or c6.w for SSLighting2)
    float4 falloff;  // x: 1 for (1 - d^16 c6.z) / d^2 (Lighting_*_NA), 0 for 1 / d^2 - d^2 c6.w (SSLighting2_*); y: a point's added distance (c6.x);
                     // z: a point's least distance (cPSMaterial c7.x); w: the radius its shadow rays spread over
    float4 shadow;   // x: a spot the game shadows with a shadow map: how far along its axis that map starts, metres; 0 for none
    float4 volumeLow;  // xyz: the lowest corner in the world of the game's volume for it (its box, a spot's cone within one); w: 1 when known
    float4 volumeHigh; // xyz: its highest corner
};

struct GIFrameConstants
{
    GI_ROW_MAJOR float4x4 viewProjection; // World to clip, jittered as the depth was drawn
    GI_ROW_MAJOR float4x4 viewProjectionInverse;
    GI_ROW_MAJOR float4x4 previousViewProjection;
    float4 eye;          // xyz: the camera in the world
    float4 sunDirection; // xyz: towards the sun, world space
    float4 sunColor;     // rgb: the sun's diffuse light at normal incidence, in the game's units
    float4 skySH[9];     // rgb: the sky's radiance as L2 spherical harmonics, world space
    uint4 dimensions;    // xy: render size; z: frame index; w: instance count
    float4 nearFar;      // x: near plane; y: far plane; z: albedo where none is known; w: output scale (the exposure)
    uint4 counts;        // x: the frame's new geometries (ClearNewGeometries)
    // The light (gi_light.hlsl): its rays, from a grid half the render size each way, and the hash grid
    // cache (AMD Capsaicin's HashGridCacheConstants).
    uint4 lightLayout;        // xy: the light's size; z: its pixels; w: frame parity
    uint4 lightRays;          // x: rays a light pixel, 1, 2 or 4, twice on foliage (TraceLight)
    float4 cellSizes;         // y: hash cell size; z: smallest hash cell; w: hash tile size
    uint4 hashCounts;         // x: buckets; y: tiles; z: cells; w: tiles per bucket
    uint4 hashTileSizes;      // Cells across a tile, mips 0 to 3
    uint4 hashTileCells;      // Cells in a tile, mips 0 to 3
    uint4 hashTileFirstCells; // First cell of each mip in a tile
    uint4 hashMisc;           // x: cells in a tile, all mips; y: packed tile parity; z: samples a cell takes a frame
    float4 hashSamples;       // x: most samples; y: chance a multibounce ray is not traced; z: most multibounce samples; w: tile cell ratio
    // The sun's ray-traced shadows (gi_shadow.hlsl).
    float4 shadowRange;  // The game's shadow map range (cPSLight): x: centre depth (c8.x); y: start distance (c8.y); z: fade rate (c6.x); w: 1 once known
    float4 shadowParams; // x: tangent of the sun's angular radius (SunSample); y: how much sun an alpha-tested mesh blocks; z: ray length; w: 1 on a fresh start
    uint4 objectMotion;  // x: 1 when g_ObjectVelocity (gi_common.hlsli) holds this frame's velocity of the game's moving meshes
    uint4 shadowDiagnostics; // x: 1 for every surface casting from both sides (ShadowRayTowards); y: 1 for the debug view of where the shadow rays leave (ShadowStartColor)
    uint4 lightDiagnostics;  // z: 1 for the denoiser's history into g_LightDebug (CompositeLight)
    // The game's terrain (TerrainGrid in gi_scene.hlsl): a world point is (u, height, v) x scale +
    // offset, u and v its virtual heightmap coordinates; the page table's converter (the terrain
    // shaders' b4 c0 and c1).
    float4 terrainScale;        // xyz: the terrain shaders' b5 c10
    float4 terrainOffset;       // xyz: b5 c11
    float4 terrainUvConverter;  // b4 c0
    float4 terrainTexelCorrect; // xy: b4 c1.xy
    float4 terrainRings[GI_TERRAIN_RINGS]; // xy: the ring's lowest corner (world x, z); z: cell size; w: cells across
    uint4 terrainRingOffsets;   // Each ring's first vertex, in bytes into the geometry pool
    // Ray cones for the textures' levels of detail (gi_common.hlsli, TextureLod): x the angle a pixel
    // spans, radians.
    float4 rayCone;
    // The upscaler's jitter in NDC as the depth was drawn (cPSScene c28, m_cameraCenterOffset): xy this
    // frame's, zw the last frame's.
    float4 jitter;
    // The point and spot lights' shadows (TraceLocalLightShadows): x the frame's lights, y rays a pixel.
    uint4 localLightCount;
    GILocalLight localLights[GI_MAX_LOCAL_LIGHTS];
};

// One mesh of a character posed this frame (Skin in gi_scene.hlsl): its bind pose (16 bytes a vertex:
// the position, then the texture coordinates) and skin (16 bytes a vertex: four 16-bit bone numbers,
// then four 8-bit unorm weights) in the pool, and where its posed vertices go, in the bind pose's
// layout. Records are sorted by their first vertex in the frame's dispatch and adjoin.
struct GISkinRecord
{
    uint firstVertex;  // In the frame's dispatch
    uint vertexCount;
    uint bindOffset;   // Bytes into the geometry pool
    uint skinOffset;
    uint posedOffset;
    uint firstBone;    // The character's palette: three float4 rows a bone from this one, in g_Palettes
    uint padding[2];
};

// One geometry of a bottom-level structure (gi_scene.hpp): where its triangles lie in the geometry
// pool, and its material. An instance's geometries start at its InstanceContributionToHitGroupIndex,
// in its structure's order.
struct GIGeometry
{
    uint vertexOffset;    // Bytes into the pool: 16 a vertex, xyz its position, w its texture coordinates with GI_GEOMETRY_UV
    uint indexOffset;     // Bytes into the pool: 16-bit indices, three a triangle; or GI_NO_INDICES
    uint flags;           // GI_GEOMETRY_*
    uint texture;         // 1 + its base texture's index in g_SceneTextures (its colour, and a cut-out geometry's alpha), 0 without
    float alphaThreshold; // With GI_GEOMETRY_ALPHA_TEXTURE: the alpha below which light passes
    uint padding[3];
};

// The shadow probe: the sun ray from the screen's centre pixel, as the shadow rays trace it but
// towards the sun's centre, with everything that decides it (ProbeShadows in gi_shadow.hlsl).
struct GIShadowProbe
{
    float4 receiver; // xyz: the centre pixel's surface, world space; w: its depth (0 is the sky)
    float4 normal;   // xyz: its normal; w: its distance from the eye
    float4 ray;      // xyz: towards the sun's centre; w: how far the pixel lies beyond the game's shadow map (0 to 1)
    float4 segments; // x: the near segment's end; y: the copy tolerance; z: the near hit's distance; w: the far hit's distance
    uint4 hits;      // x: copies of the surface skipped near; y: the near hit's instance + 1; z: the far hit's + 1; w: the whole ray's first hit past
                     // copies of the surface, cut-outs without their texture letting it through, + 1
    uint4 hitGeometry; // Of that last hit: x its geometry's row of the table, y its kind (GI_INSTANCE_*) | its geometry's flags << 16,
                       // z its geometry in its instance | 1 << 31 on a front face, w asuint(its distance)
    float4 terrain;  // Of the same on the terrain, its triangle's highest corner: x its height (as the ring was last made), y the heightmap's there
                     // now, z the level of detail resident there now; w the frame's index (GIFrameConstants::dimensions.z)
    // The centre pixel's global illumination rays (GI_PROBE_LIGHT_RAYS, cosine-weighted, traced as the probes' are):
    uint4 lightRays;    // x those that hit within 1 m, y within 10 m, z that get away, w that hit a back face
    uint4 lightNearest; // Their nearest hit, and below the camera's ray through the centre pixel (the scene view's): x its
                        // instance + 1, y its geometry | 1 << 31 on a front face, z its kind (GI_INSTANCE_*), w asuint(its distance)
    uint4 view;
    float4 shadow;   // x: the denoised shadow at the centre; y: this frame's ray (1 lit, 0 blocked)
    // Where the shadow rays start (ShadowSurfaceAt, ShadowRayStart):
    float4 start;    // xyz: the surface's normal towards the eye; w: 1 on the scene's own surface, 0 at the depth's point
    float4 lift;     // xyz: the start's lift off that surface, world space; w: 1 when the rays leave its far side
    uint4 startHit;  // On the scene's own surface, its triangle: x its instance + 1, y its geometry, z its primitive; w the
                     // primitive of the whole ray's first hit (hits.w)
    uint4 crossed;   // The first thing between the lifted start and the surface, from the start, as lightNearest; zeros for none
};

// The lamp probe: the point and spot lights at the screen's centre pixel, each with a ray from there to
// its centre as their shadow rays trace them (ProbeLocalLights in gi_shadow.hlsl); the strongest
// GI_PROBE_LIGHTS kept, most light first. Works with or without a sun.
#define GI_PROBE_LIGHTS 6u
#define GI_PROBE_LIGHT_HITS 4u
struct GILightProbe
{
    float4 receiver; // xyz: the centre pixel's surface, world space; w: its distance from the eye, 0 for the sky
    float4 share;    // x: the share of the lights' light the rays to their centres let through; y: the filtered share the light
                     // pass reads there; z: all the lights' light there without shadows (luminance); w: how many lights light it
    float4 start;    // Where the rays leave (ShadowSurfaceAt): xyz the scene surface's point less the depth's; w 1 on the scene's own surface
    uint4 startHit;  // That surface, as GIShadowProbe::lightNearest; zeros at the depth's point
    uint4 view;      // The camera's ray through the centre pixel, as the scene view traces it (GIShadowProbe::view)
    float4 lights[GI_PROBE_LIGHTS]; // x: the light's index in the frame + 1, 0 for none; y: its light there without shadows
                                    // (luminance); z: how far its centre is; w: where the ray to it first meets what takes its
                                    // light (what blocks it, or hair), metres, or -1
    float4 through[GI_PROBE_LIGHTS]; // x: the share of that light the ray gets past it with: 0 blocked, less than 1 past hair
    // What blocks that ray, as GIShadowProbe::lightNearest, and what lies past it towards the light, GI_PROBE_LIGHT_HITS a
    // light; zeros past the last
    uint4 hits[GI_PROBE_LIGHTS * GI_PROBE_LIGHT_HITS];
};

// D3D12_RAYTRACING_INSTANCE_DESC: the frame's instances as the top level holds them, which the
// kernels read too.
struct GIInstanceDesc
{
    float4 transform[3];   // Object to world, rows applied as dot(row, float4(v, 1))
    uint idAndMask;        // InstanceID (GI_INSTANCE_*) in the low 24 bits, InstanceMask (GI_MASK_SCENE) in the high 8
    uint hitGroupAndFlags; // InstanceContributionToHitGroupIndex (its first geometry's slot) in the low 24, flags in the high 8
    uint2 blas;
};

#ifdef __cplusplus
static_assert(sizeof(GIFrameConstants) % 16 == 0);
static_assert(sizeof(GIInstanceDesc) == 64);
static_assert(sizeof(GIGeometry) == 32);
}
#endif

#endif
