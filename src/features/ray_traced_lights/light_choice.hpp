#pragma once

#include "game_constants.hpp"
#include "game_lights.hpp"

#include <d3d11.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Which of the game's point and spot lights a kick gives the rays, the chosen lights (docs/DEVELOPMENT.md,
// "Ray-traced lights", "Which lights"), and each as the rays take it: in world space, with its range, its
// cone and where its shadow rays end. All from what a light's lighting draw had; local_lights.hpp records
// the draws and draws the lights again.
namespace phantom_fx::light_choice
{
// A light as its lighting draw had it.
struct DrawnLight
{
    const game_lights::GameLight* game{};
    game_constants::SceneConstants scene{};
    game_constants::LampLightConstants light{};
    game_constants::LampMaterialConstants material{};
    D3D11_VIEWPORT viewport{};
};

// A point or spot light for its shadow rays and the global illumination's hits (GILocalLight), world
// space: its light as the game's light shaders work it out, cut off past a range, and a spot's outside a
// cone.
struct LocalLight
{
    std::array<float, 3> position{}, axis{};
    float range{};
    float cosOuter{};  // The cosine of a spot's cone's outer edge; below -1 for a point
    float clearance{}; // How far short of the light its rays end: the lamp around it
    float penumbraScale{}, coneExponent{}; // A spot's cone: (cosine - cosOuter) x penumbraScale, saturated, to this power
    float maxFalloff{};                    // The most a spot's falloff gives
    std::array<float, 3> color{};          // Before exposure, as the game's cPSLight c4
    bool sixteenth{};                      // (1 - d^16 factor) / d^2 (Lighting_*_NA); else 1 / d^2 - d^2 factor (SSLighting2_*)
    float factor{};
    float addedDistance{}, leastDistance{}; // A point's: its distance is at least the least, plus the added
    float radius{};                         // Its shadow rays spread over a disc this wide around it
    float shadowStart{};                    // A shadow-mapped spot's: how far along its axis the game's shadow map starts; 0 for none
    // The bounds in the world of the game's volume for it (its box, a spot's cone within one), when known:
    // its light stops there, as the game's does.
    std::array<float, 3> volumeLowest{}, volumeHighest{};
    bool volumeKnown{};
    // Path tracing (direct_light.hlsl): the light's own scales (cPSLight c8), its specular's (1 where its own
    // shader has none, as the ray-traced lights draw it) and its diffuse's.
    float specularScale{1}, diffuseScale{1};
    float size{}; // cPSLight c4.w: its specular highlight is of a disc this wide
    // A masked spot's mask (the game's t4) projected as its shader projects it, from the world: the rows for x,
    // y and w (cPSScene c16, c17 and c19 through the light pass's view); its tile in the mask atlas, from 1, 0
    // for none (ray_tracing.cpp).
    std::array<std::array<float, 4>, 3> maskRows{};
    bool isMasked{};
    std::uint32_t maskTile{};
};

// The bounds a light's volume had in the world: lowest, then highest corner.
using Bounds = std::array<float, 6>;

// The light's reach: the pixels of its viewport it can light, its sphere, or for a spot the bounding
// sphere of its cone, on screen (game_lights::sphereOnScreen). The whole viewport when the camera plane
// cuts the sphere; none when it is behind or off screen, and a light that reaches none is never drawn.
std::optional<D3D11_RECT> reachOf(const DrawnLight& light);

struct Choice
{
    std::vector<std::size_t> taken; // Indices into `lights`, best first
    // Left for another frame's camera, and left past the light cap.
    std::size_t otherFrame{}, overCap{};
};

// The chosen lights among `lights` for the frame `camera` draws: those drawn from its eye (within a
// millimetre) that reach any pixel, most pixels first, then the nearest, then by key (keyOf), up to the
// light cap (GI_MAX_LOCAL_LIGHTS). Past it, those reaching the fewest go without, the same ones frame
// after frame, whatever order the game drew them in.
Choice choose(std::span<const DrawnLight* const> lights, const game_constants::SceneConstants& camera);

// A light the same from frame to frame: its kind, where it is in the world to the centimetre, and its colour.
std::uint64_t keyOf(const DrawnLight& light);

// The light as the rays take it, stopped at `volume` when its bounds are known.
LocalLight forRays(const DrawnLight& light, const std::optional<Bounds>& volume);
}
