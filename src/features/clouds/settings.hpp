#pragma once

#include "features/setting_visitor.hpp"

#include <cstdint>

namespace phantom_fx
{
// How finely the clouds are marched: more steps resolve the edges and the cauliflower detail instead of smoothing them.
enum class CloudQuality : std::uint8_t { Low, Medium, High, Ultra };

// Better clouds (clouds.hpp): the game's cloud draw raymarched with more steps, shaped by the same weather.
struct Clouds
{
    bool on{};
    CloudQuality quality{CloudQuality::High};
    float coverage{1.6f}; // The weather's own coverage, a share of it below 100% and towards overcast above, in the same weather
    float size{1.5f}; // Larger is wider clouds, with the same sky filled by bigger shapes
    float shadow{1};   // How dark the clouds' shadow on the ground is; 0 leaves the ground alone
    float wind{1};     // How fast they drift, 1 being the game's real wind
    bool fog{true};    // The game's fog over the clouds, as the terrain gets it: off keeps a distant cloud at its own colour

    bool operator==(const Clouds&) const = default;
};

void describe(Clouds& clouds, SettingVisitor& visit);
}
