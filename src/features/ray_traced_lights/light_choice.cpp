// Which lights a kick gives the rays, and each as they take it; light_choice.hpp describes it.
#include "features/ray_traced_lights/light_choice.hpp"

#include "passes/ray_tracing/shaders/gi_shared.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace phantom_fx::light_choice
{
namespace
{
using namespace game_lights;
using game_constants::SceneConstants;

// A light sits inside its lamp's model: a ray ends this far short of it, or the light's size if larger.
constexpr float LightClearance = 0.1f;
// The radius a light's shadow rays spread over where the game gives it no size, or shadows it with a
// shadow map, whose four-sample filter is sharp whatever its size (which only widens its specular): a
// bulb's, so its shadows soften with distance as a real lamp's do.
constexpr float BulbRadius = 0.03f;
// GILocalLight::axis.w of a point light: below any cosine, so every way is in its "cone".
constexpr float PointCone = -2.0f;

std::array<float, 3> normalized(std::array<float, 3> v)
{
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0)
        for (auto& value : v) value /= length;

    return v;
}

// How far along a shadow-mapped spot's axis the game's shadow map starts: where shadowProjection gives
// the map's near depth, 1 (its depth is reversed: cleared to 0, tested greater-equal). Nothing nearer the
// light is in the map, so the game's shadows never see the recess its lamp sits in; the corridor's lamps
// in the prologue's hospital sit 13 cm above its ceiling, in a box the rays met 0.3 to 0.6 m short of
// them (the lamp probe, 2026-10-26). 0 without a shadow map, or where the projection gives none within
// the light's reach.
float shadowMapStart(const DrawnLight& light)
{
    if (!light.game->shadowMap || light.game->shape != Shape::Spot) return 0;

    const float* depthRow = light.scene.shadowProjection[2];
    const float* wRow = light.scene.shadowProjection[3];
    const float* position = light.light.position;
    const auto axis = normalized({light.light.axis[0], light.light.axis[1], light.light.axis[2]});
    const auto atLight = [&](const float* row) { return row[0] * position[0] + row[1] * position[1] + row[2] * position[2] + row[3]; };
    const auto alongAxis = [&](const float* row) { return row[0] * axis[0] + row[1] * axis[1] + row[2] * axis[2]; };
    const float start = (atLight(wRow) - atLight(depthRow)) / (alongAxis(depthRow) - alongAxis(wRow));

    return std::isfinite(start) && start > 0 && start < rangeOf(light.light, *light.game) ? start : 0;
}

// Drawn for the frame `camera` draws: the same eye, within a millimetre. Lists recorded before the kick
// are this frame's, unless one was left from an earlier frame or the next frame's recording started early.
bool sameFrame(const DrawnLight& light, const SceneConstants& camera)
{
    for (int i = 0; i < 3; ++i)
        if (std::abs(light.scene.eye[i] - camera.eye[i]) > 1e-3f) return false;

    return true;
}
}

std::optional<D3D11_RECT> reachOf(const DrawnLight& light)
{
    const float range = rangeOf(light.light, *light.game);
    const float* position = light.light.position;
    std::array<float, 3> centre{position[0], position[1], position[2]};
    float radius = range;

    // Up to a 60 degree cone, the sphere through its apex and the rim of its end holds it all.
    const float cosine = light.light.cutOffCosine;
    if (light.game->shape == Shape::Spot && cosine >= 0.5f)
    {
        const auto axis = normalized({light.light.axis[0], light.light.axis[1], light.light.axis[2]});
        radius = range / (2 * cosine);
        for (int i = 0; i < 3; ++i) centre[i] += axis[i] * radius;
    }

    const auto& viewport = light.viewport;
    const LONG left = static_cast<LONG>(viewport.TopLeftX), top = static_cast<LONG>(viewport.TopLeftY);
    const std::array<float, 2> size{viewport.Width, viewport.Height};
    const D3D11_RECT whole{left, top, left + static_cast<LONG>(size[0]), top + static_cast<LONG>(size[1])};
    const float z = centre[2];
    if (z + radius <= 0) return std::nullopt;
    if (z - radius <= 0) return whole;

    const auto rect = sphereOnScreen(light.scene, centre, radius, size);
    if (!rect) return std::nullopt;

    return D3D11_RECT{left + rect->min[0], top + rect->min[1], left + rect->max[0], top + rect->max[1]};
}

Choice choose(std::span<const DrawnLight* const> lights, const SceneConstants& camera)
{
    struct Reaching
    {
        LONG pixels;
        float distance;
        std::uint64_t key;
        std::size_t index;
    };

    Choice choice;
    std::vector<Reaching> reaching;
    for (std::size_t i = 0; i < lights.size(); ++i)
    {
        const auto& light = *lights[i];
        if (!sameFrame(light, camera))
        {
            ++choice.otherFrame;
            continue;
        }

        const auto reach = reachOf(light);
        if (!reach) continue;

        const float* position = light.light.position;
        reaching.push_back({(reach->right - reach->left) * (reach->bottom - reach->top),
            std::sqrt(position[0] * position[0] + position[1] * position[1] + position[2] * position[2]), keyOf(light), i});
    }

    // Exact ties go by the key, so the choice depends on which lights there are, not on the order the game
    // drew them in.
    std::sort(reaching.begin(), reaching.end(), [](const Reaching& a, const Reaching& b) {
        if (a.pixels != b.pixels) return a.pixels > b.pixels;
        if (a.distance != b.distance) return a.distance < b.distance;

        return a.key < b.key;
    });

    if (reaching.size() > GI_MAX_LOCAL_LIGHTS)
    {
        choice.overCap = reaching.size() - GI_MAX_LOCAL_LIGHTS;
        reaching.resize(GI_MAX_LOCAL_LIGHTS);
    }

    for (const auto& each : reaching) choice.taken.push_back(each.index);

    return choice;
}

std::uint64_t keyOf(const DrawnLight& light)
{
    const auto at = game_constants::toWorld(light.scene, light.light.position, 1);
    std::uint64_t key = reinterpret_cast<std::uintptr_t>(light.game);
    const auto mix = [&](std::uint64_t value) { key = (key ^ value) * 0x100000001B3ull; };
    for (const float coordinate : at) mix(static_cast<std::uint64_t>(std::llround(coordinate * 100)));
    for (const float channel : light.light.color) mix(std::bit_cast<std::uint32_t>(channel));

    return key;
}

// A masked spot's mask projection (cPSScene c16, c17, c19, applied to the light pass's view position) from
// the world: each row through the light pass's view, the inverse of its view to world (c8 to c10), a
// rotation and a translation.
std::array<std::array<float, 4>, 3> maskRowsInWorld(const game_constants::SceneConstants& scene)
{
    const auto& toWorld = scene.view;
    const auto& projection = scene.shadowProjection2;
    const int rows[]{0, 1, 3};
    std::array<std::array<float, 4>, 3> world{};
    for (int i = 0; i < 3; ++i)
    {
        const auto& row = projection[rows[i]];
        double translation = row[3];
        for (int column = 0; column < 3; ++column)
        {
            double sum = 0;
            for (int k = 0; k < 3; ++k) sum += double{row[k]} * toWorld[column][k];
            world[i][column] = static_cast<float>(sum);
            for (int k = 0; k < 3; ++k) translation -= double{row[k]} * toWorld[column][k] * toWorld[column][3];
        }

        world[i][3] = static_cast<float>(translation);
    }

    return world;
}

LocalLight forRays(const DrawnLight& light, const std::optional<Bounds>& volume)
{
    const bool spot = light.game->shape == Shape::Spot;
    const bool sixteenth = light.game->falloff == Falloff::Sixteenth;
    const auto& constants = light.light;
    const auto& falloff = constants.falloff;

    LocalLight rays{.position = game_constants::toWorld(light.scene, constants.position, 1),
        .axis = spot ? normalized(game_constants::toWorld(light.scene, constants.axis, 0)) : std::array<float, 3>{0, 1, 0},
        .range = rangeOf(constants, *light.game),
        .cosOuter = spot ? constants.cutOffCosine : PointCone,
        .clearance = std::max(LightClearance, constants.size),
        .penumbraScale = spot ? constants.penumbraScale : 0,
        .coneExponent = spot ? constants.coneExponent : 0,
        .maxFalloff = spot ? light.material.maxFalloff : std::numeric_limits<float>::max(),
        .color = {constants.color[0], constants.color[1], constants.color[2]},
        .sixteenth = sixteenth,
        .factor = sixteenth ? falloff[2] : falloff[3],
        .addedDistance = spot ? 0 : falloff[0],
        .leastDistance = spot ? 0 : light.material.leastDistance,
        .radius = !light.game->shadowMap && constants.size > 0 ? constants.size : BulbRadius,
        .shadowStart = shadowMapStart(light),
        .specularScale = light.game->specular ? constants.specularScale : 1,
        .diffuseScale = constants.diffuseScale,
        .size = constants.size};
    const bool isMaskedSpot = light.game->twin == game_lights::Twin::MaskedSpotNA || light.game->twin == game_lights::Twin::MaskedSpot2;
    if (isMaskedSpot)
    {
        rays.maskRows = maskRowsInWorld(light.scene);
        rays.isMasked = true;
    }

    if (volume)
    {
        rays.volumeLowest = {(*volume)[0], (*volume)[1], (*volume)[2]};
        rays.volumeHighest = {(*volume)[3], (*volume)[4], (*volume)[5]};
        rays.volumeKnown = true;
    }

    return rays;
}
}
