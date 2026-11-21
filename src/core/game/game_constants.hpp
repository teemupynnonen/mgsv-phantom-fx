#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// The game's constant buffers by name (docs/DEVELOPMENT.md, "Constant buffers", "Sun light and
// shadows"): what its shaders read at each slot, as the add-on reads it. A register (c0, c1, ...) is
// four floats; matrices are rows, applied as dot(row, v). Where the game's names are known, every
// register has its name; elsewhere only the registers the add-on reads do, and the rest are padding.
namespace phantom_fx::game_constants
{
inline constexpr std::size_t RegisterSize = 16; // Bytes

// cPSSystem, b0 (64 bytes).
struct SystemConstants
{
    static constexpr std::uint32_t Slot = 0;

    float param[4];
    float renderInfo[4];       // xy: the render size
    float renderBuffer[4];     // The render size, then its inverse
    float dominantLightDir[4];
};

static_assert(sizeof(SystemConstants) == 4 * RegisterSize);
static_assert(offsetof(SystemConstants, renderInfo) == 1 * RegisterSize && offsetof(SystemConstants, renderBuffer) == 2 * RegisterSize
    && offsetof(SystemConstants, dominantLightDir) == 3 * RegisterSize);

// cVSScene / cPSScene, b2 (480 bytes): the camera, the exposure and the fog a pass draws with. A light
// pass's differ from the main view's: its view takes view space to the world.
struct SceneConstants
{
    static constexpr std::uint32_t Slot = 2;

    float projectionView[4][4];               // c0-c3: P x V
    float projection[4][4];                   // c4-c7: reversed depth, A and B in row 2
    float view[4][4];                         // c8-c11: world to view; in a light pass view to world
    float shadowProjection[4][4];             // c12-c15: in a lamp pass with a shadow map, view space to its map
    float shadowProjection2[4][4];            // c16-c19: in the main view, the previous frame's P x V
    float eye[4];                             // c20: m_eyepos, w 1
    float projectionParam[4];                 // c21: 1 / P00, 1 / P11, B, A
    float width, height, nearPlane, farPlane; // c22: m_viewportSize
    float exposureXY[2], exposure, exposureW; // c23: m_exposure
    float fogParam[3][4];                     // c24-c26
    float fogColor[4];                        // c27
    float cameraCenterOffset[4];              // c28: xy the upscaler's jitter in NDC (camera::applyJitter)
    float shadowMapResolutions[4];            // c29
};

static_assert(sizeof(SceneConstants) == 30 * RegisterSize);
static_assert(offsetof(SceneConstants, projection) == 4 * RegisterSize && offsetof(SceneConstants, view) == 8 * RegisterSize
    && offsetof(SceneConstants, shadowProjection) == 12 * RegisterSize && offsetof(SceneConstants, shadowProjection2) == 16 * RegisterSize
    && offsetof(SceneConstants, eye) == 20 * RegisterSize && offsetof(SceneConstants, projectionParam) == 21 * RegisterSize
    && offsetof(SceneConstants, width) == 22 * RegisterSize && offsetof(SceneConstants, nearPlane) == 22 * RegisterSize + 8
    && offsetof(SceneConstants, exposure) == 23 * RegisterSize + 8 && offsetof(SceneConstants, fogParam) == 24 * RegisterSize
    && offsetof(SceneConstants, fogColor) == 27 * RegisterSize && offsetof(SceneConstants, cameraCenterOffset) == 28 * RegisterSize
    && offsetof(SceneConstants, shadowMapResolutions) == 29 * RegisterSize);

// cPSLight, b3 (176 bytes), in the sun passes; view space. A lamp pass has its own at the same slot and
// size (LampLightConstants): the type a reader asks for is its claim about which light it reads.
struct SunLightConstants
{
    static constexpr std::uint32_t Slot = 3;

    float direction[3], padding0;                          // c0: towards the sun
    float color[3], padding1;                              // c1: before the exposure
    float padding2[2], skyReflectionFloor, padding7;       // c2.z: the least F0 its sky reflection takes
    float padding8[3][4];                                  // c3-c5
    float shadowRangeFade, padding3[3];                    // c6
    float padding4[4];                                     // c7
    float shadowRangeCentre, shadowRangeStart, padding5[2]; // c8: the range's centre depth, the distance it starts at
    float padding6[2][4];                                  // c9-c10
};

static_assert(sizeof(SunLightConstants) == 11 * RegisterSize);
static_assert(offsetof(SunLightConstants, color) == 1 * RegisterSize && offsetof(SunLightConstants, skyReflectionFloor) == 2 * RegisterSize + 8
    && offsetof(SunLightConstants, shadowRangeFade) == 6 * RegisterSize
    && offsetof(SunLightConstants, shadowRangeCentre) == 8 * RegisterSize && offsetof(SunLightConstants, shadowRangeStart) == 8 * RegisterSize + 4);

// cPSLight, b3 (176 bytes), in the point and spot light passes (game_lights.hpp); view space.
struct LampLightConstants
{
    static constexpr std::uint32_t Slot = 3;

    float padding0[3][4];                                      // c0-c2
    float position[3], padding1;                               // c3
    float color[3], size;                                      // c4
    float axis[3], padding2;                                   // c5: the way a spot points
    float falloff[4];                                          // c6: see game_lights::Falloff
    float cutOffCosine, penumbraScale, coneExponent, padding3; // c7: a spot's cone; y 1 / its penumbra
    float specularScale, diffuseScale, padding4[2];            // c8: z the shadow's strength
    float padding5[2][4];                                      // c9-c10
};

static_assert(sizeof(LampLightConstants) == 11 * RegisterSize);
static_assert(offsetof(LampLightConstants, position) == 3 * RegisterSize && offsetof(LampLightConstants, color) == 4 * RegisterSize
    && offsetof(LampLightConstants, size) == 4 * RegisterSize + 12 && offsetof(LampLightConstants, axis) == 5 * RegisterSize
    && offsetof(LampLightConstants, falloff) == 6 * RegisterSize && offsetof(LampLightConstants, cutOffCosine) == 7 * RegisterSize
    && offsetof(LampLightConstants, penumbraScale) == 7 * RegisterSize + 4 && offsetof(LampLightConstants, coneExponent) == 7 * RegisterSize + 8
    && offsetof(LampLightConstants, specularScale) == 8 * RegisterSize);

// cPSMaterial, b4 (128 bytes), in the point and spot light passes.
struct LampMaterialConstants
{
    static constexpr std::uint32_t Slot = 4;

    float padding0[7][4];                         // c0-c6
    float leastDistance, padding1[2], maxFalloff; // c7: a point's least distance, the most a spot's falloff gives
};

static_assert(sizeof(LampMaterialConstants) == 8 * RegisterSize);
static_assert(offsetof(LampMaterialConstants, leastDistance) == 7 * RegisterSize && offsetof(LampMaterialConstants, maxFalloff) == 7 * RegisterSize + 12);

// The material constants, b4 (128 bytes), of the terrain's vertex shaders (NewTerrain_*).
struct TerrainMaterialConstants
{
    static constexpr std::uint32_t Slot = 4;

    float pageTableConverter[2][4]; // c0: the uv converter; c1.xy: the texel correction
    float padding[6][4];            // c2-c7
};

static_assert(sizeof(TerrainMaterialConstants) == 8 * RegisterSize);

// cVSObject / cPSObject, b5 (208 bytes): the object a draw draws.
struct ObjectConstants
{
    static constexpr std::uint32_t Slot = 5;

    float viewWorld[4][4];   // c0-c3: object to view
    float world[4][4];       // c4-c7: object to world, not kept current
    float useWeightCount[4]; // c8: x set on skinned draws
    float localParam[4][4];  // c9-c12: the shader's own; in a terrain draw c10 the world's scale, c11 its offset
};

static_assert(sizeof(ObjectConstants) == 13 * RegisterSize);
static_assert(offsetof(ObjectConstants, world) == 4 * RegisterSize && offsetof(ObjectConstants, useWeightCount) == 8 * RegisterSize
    && offsetof(ObjectConstants, localParam) == 9 * RegisterSize);

// VSBones, b6 (3072 bytes): a skinned draw's bones, 3x4 rows each, the model's palette through the mesh's
// bone group.
struct BonesConstants
{
    static constexpr std::uint32_t Slot = 6;

    float bones[32][3][4];         // g_vsBone
    float previousBones[32][3][4]; // g_vsPrevBone
};

static_assert(sizeof(BonesConstants) == 192 * RegisterSize);
static_assert(offsetof(BonesConstants, previousBones) == 96 * RegisterSize);

// PsWork's constants, b7 (576 bytes), in the sky probe pass (SH_SphereMap): light as spherical harmonics,
// a 4x4 matrix per colour (gi_lighting.hpp).
struct ProbeConstants
{
    static constexpr std::uint32_t Slot = 7;

    float paramSH[3][16];    // c0-c11: g_psParamSH, the probe's light
    float padding0[4][4];    // c12-c15: its sky occlusion
    float paramSHSky[3][16]; // c16-c27: g_psParamSHSky, the sky's
    float padding1[8][4];    // c28-c35
};

static_assert(sizeof(ProbeConstants) == 36 * RegisterSize);
static_assert(offsetof(ProbeConstants, paramSHSky) == 16 * RegisterSize);

// A light pass's view-space point (`w` 1) or direction (`w` 0) in the world.
inline std::array<float, 3> toWorld(const SceneConstants& lightPass, const float (&v)[3], float w)
{
    std::array<float, 3> world{};
    for (int row = 0; row < 3; ++row)
    {
        const auto& m = lightPass.view[row];
        world[row] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3] * w;
    }

    return world;
}
}
