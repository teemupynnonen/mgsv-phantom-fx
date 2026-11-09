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

// A light sits inside its lamp's model: a ray ends this far short of it, or the light's size if larger. At 10 cm a wall
// lamp's own fitting (Africa's villa, model 84a1909ed59bcd81) blocked its upward spot 17 to 20 cm out, all of its light on
// the wall above, which the game, unshadowed, lights (the lamp probe, 2026-11-07).
constexpr float LightClearance = 0.25f;
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

Choice choose(std::span<const DrawnLight* const> lights, const SceneConstants& camera, std::span<const std::uint64_t> placesTaken)
{
    struct Reaching
    {
        LONG pixels;
        float distance;
        std::uint64_t key;
        std::uint64_t place; // Its light's place to the centimetre: two spots at one lamp share it
        std::size_t index;
        float rank; // Its pixels, a fifth more when the rays had its place last time
    };

    const auto wasTaken = [placesTaken](std::uint64_t place) { return std::ranges::find(placesTaken, place) != placesTaken.end(); };

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

        // Ranked by its reach without the upscaler's jitter: under a pixel, it moved a distant lamp's few
        // pixels' edges, and the lamps at the cap swapped every frame with the camera still.
        auto unjittered = light;
        unjittered.scene.cameraCenterOffset[0] = unjittered.scene.cameraCenterOffset[1] = 0;
        const auto reach = reachOf(unjittered);
        if (!reach) continue;

        const float* position = light.light.position;
        // Its place in the world to the centimetre: a lamp drawn as two spots, one up and one down, is one place, and the
        // two must come and go together. Traced one frame and left to the game's unshadowed draw the next, the wall they
        // light changed brightness every few frames with the camera still (2026-11-08).
        const auto at = game_constants::toWorld(light.scene, light.light.position, 1);
        std::uint64_t place = 1469598103934665603ull; // FNV-1a, so two places cannot share a hash by adding up
        for (const float coordinate : at)
        {
            place ^= static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(coordinate * 100)));
            place *= 1099511628211ull;
        }
        const LONG pixels = (reach->right - reach->left) * (reach->bottom - reach->top);
        reaching.push_back({pixels, std::sqrt(position[0] * position[0] + position[1] * position[1] + position[2] * position[2]),
            keyOf(light), place, i, static_cast<float>(pixels) * (wasTaken(place) ? 1.2f : 1.0f)});
    }

    // Exact ties go by the key, so the choice depends on which lights there are, not on the order the game
    // drew them in.
    std::ranges::stable_sort(reaching, [](const Reaching& a, const Reaching& b) {
        if (a.rank != b.rank) return a.rank > b.rank;
        if (a.pixels != b.pixels) return a.pixels > b.pixels;
        if (a.distance != b.distance) return a.distance < b.distance;

        return a.key < b.key;
    });

    // Whole places, up to the cap: one whose lights do not all fit is left for the game's draw, all of it, rather than
    // half traced. `handled` keeps its lights from being asked about again; only the places actually taken go back in
    // `choice.places`, for the next call's bonus.
    std::vector<std::uint64_t> handled;
    for (const auto& each : reaching)
    {
        if (std::ranges::find(handled, each.place) != handled.end()) continue; // Its place was already answered
        handled.push_back(each.place);

        const auto members = std::ranges::count(reaching, each.place, &Reaching::place);
        if (choice.taken.size() + static_cast<std::size_t>(members) > GI_MAX_LOCAL_LIGHTS)
        {
            choice.overCap += static_cast<std::size_t>(members);
            continue;
        }

        choice.places.push_back(each.place);
        for (const auto& member : reaching)
            if (member.place == each.place) choice.taken.push_back(member.index);
    }

    return choice;
}

std::uint64_t keyOf(const DrawnLight& light)
{
    const auto at = game_constants::toWorld(light.scene, light.light.position, 1);
    std::uint64_t key = reinterpret_cast<std::uintptr_t>(light.game);
    const auto mix = [&](std::uint64_t value) { key = (key ^ value) * 0x100000001B3ull; };
    for (const float coordinate : at) mix(static_cast<std::uint64_t>(std::llround(coordinate * 100)));
    for (const float channel : light.light.color) mix(std::bit_cast<std::uint32_t>(channel));
    // Where a spot points, to the hundredth: a wall lamp in Africa's villa is two spots of one kind and colour at one place,
    // one up and one down (the lamp probe, 2026-11-07). With one key, each took the other's volume bounds, and its light
    // was cut to them: the wall lit above the lamp one frame and below it the next.
    if (light.game->shape == Shape::Spot)
        for (const float component : normalized(game_constants::toWorld(light.scene, light.light.axis, 0)))
            mix(static_cast<std::uint64_t>(std::llround(component * 100)));

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
