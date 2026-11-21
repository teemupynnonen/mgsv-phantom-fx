#pragma once

#include "game_constants.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

// The game's point and spot lights as the shaders of its light pass draw them (docs/DEVELOPMENT.md,
// "Ray-traced lights"; each shader's row is in game_shaders.hpp), and what their constants
// (game_constants::LampLightConstants) say. Lamp contact shadows (lamp_contact_shadows.hpp) and the
// ray-traced lights (local_lights.hpp) both work with them.
namespace phantom_fx::game_lights
{
enum class Shape : std::uint8_t { Point, Spot };
// How a light's shader fades its light to zero with the distance d to the light, from the disassembly
// (docs/DEVELOPMENT.md, "Screen-space shadows"). A point adds cPSLight c6.x to d first; a spot does not.
// - Quartic (SSLighting2_*): 1 / d^2 - d^2 c6.w, zero at d = c6.w^(-1/4).
// - Sixteenth (Lighting_*_NA): (1 - d^16 c6.z) / d^2, zero at d = c6.z^(-1/16).
enum class Falloff : std::uint8_t { Quartic, Sixteenth };
// The game's shader for each kind of light with specular and without a shadow map (isTwin), which draws
// the lights of that kind again.
enum class Twin : std::uint8_t { PointNA, SpotNA, MaskedSpotNA, Point2, Spot2, MaskedSpot2, Count };

struct GameLight
{
    Shape shape;
    Falloff falloff;
    Twin twin;
    bool specular;       // The game's shader adds specular; without, its light's specular scale is 0
    bool shadowMap;      // The game's shader has a shadow map: its shadows are sharp, whatever the light's size
    bool flashlight{};   // The weapon flashlights' shader: every light it drew in the captures was one
};

constexpr bool isTwin(const GameLight& light)
{
    return light.specular && !light.shadowMap;
}

// How far a light reaches when its falloff does not end, metres.
constexpr float UnendingRange = 1000.0f;

// How far a light reaches, from the falloff in its constants (see Falloff). The ray-traced lights take it by
// the light's shape, never below 0, and UnendingRange where unknown; lamp contact shadows by
// eitherShapeRangeOf.
inline float rangeOf(const game_constants::LampLightConstants& light, const GameLight& game)
{
    const auto& falloff = light.falloff;
    const float factor = game.falloff == Falloff::Quartic ? falloff[3] : falloff[2];
    if (!(factor > 0)) return UnendingRange;

    const float reach = std::pow(factor, game.falloff == Falloff::Quartic ? -1.0f / 4 : -1.0f / 16);

    return std::max(game.shape == Shape::Point ? reach - falloff[0] : reach, 0.0f);
}

// How far a light reaches for lamp contact shadows, which do not look at its shape: the larger of a
// point's range and a spot's, infinite or NaN where unknown, which their trace takes as the whole screen.
inline float eitherShapeRangeOf(const game_constants::LampLightConstants& light, Falloff falloff)
{
    const auto& fade = light.falloff;
    const float reach = falloff == Falloff::Quartic ? std::pow(fade[3], -1.0f / 4) : std::pow(fade[2], -1.0f / 16);

    return reach - std::min(fade[0], 0.0f);
}

// Pixels from `min` to `max` (exclusive) on each axis.
struct PixelRect
{
    std::array<int, 2> min, max;
};

// The pixels a sphere at view-space `center` covers on a screen of `size` pixels, as the light pass's
// projection in `scene` puts it: on each axis, the planes through the eye that touch the sphere, at
// x / z (or y / z) = t, bound it. None when it is off the screen. The sphere must lie past the camera
// plane (center z > radius).
inline std::optional<PixelRect> sphereOnScreen(const game_constants::SceneConstants& scene, const std::array<float, 3>& center, float radius,
    std::array<float, 2> size)
{
    const auto& projection = scene.projectionParam;
    const auto& offset = scene.cameraCenterOffset;
    const float z = center[2];
    const float clearance = z * z - radius * radius;
    PixelRect rect{};
    for (int axis = 0; axis < 2; ++axis)
    {
        const float across = center[axis];
        const float spread = radius * std::sqrt(across * across + clearance);
        const float scale = axis == 0 ? 0.5f : -0.5f;
        const auto pixelAt = [&](float t) { return ((t / projection[axis] + offset[axis]) * scale + 0.5f) * size[axis]; };
        const float first = pixelAt((across * z - spread) / clearance), second = pixelAt((across * z + spread) / clearance);
        const auto clamped = [&](float pixel) { return static_cast<int>(std::clamp(pixel, 0.0f, size[axis])); };
        rect.min[axis] = clamped(std::floor(std::min(first, second)) - 1);
        rect.max[axis] = clamped(std::ceil(std::max(first, second)) + 1);
        if (rect.min[axis] >= rect.max[axis]) return std::nullopt;
    }

    return rect;
}
}
