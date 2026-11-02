#pragma once

#include "phantom_fx_settings.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

// The engine draws velocity for characters only while its motion blur is on, and DLSS needs that
// velocity whatever the player chose, so the add-on forces the velocity passes on. The blur itself
// is left to the game: with the player's motion blur on, its own blur draws as it always does, and
// with it off neither draws. docs/DEVELOPMENT.md, "Velocity without motion blur".
namespace phantom_fx::motion_blur
{
// Hooks the plugin on the supported build, with MinHook initialised; returns what was done, for
// the log. Without the hooks the game's own motion blur setting stays in charge.
std::string hook() noexcept;
// While on, the engine draws its velocity passes every frame; its blur follows the game's setting.
void setVelocityOnly(bool on) noexcept;
// Another effect's (VB-SSGI's temporal accumulation) asking for the velocity passes too: while on,
// the engine draws them every frame, whatever the upscaler asks. The blur still follows the game's
// setting and the add-on's toggle.
void wantVelocityPasses(bool on) noexcept;
// While frame generation runs the blur is halved, as NVIDIA's DLSS-G guide asks: each frame shows half
// the motion.
void setFrameGeneration(bool on) noexcept;
// Its setting off keeps the engine's blur out even with the upscaler on; the strength multiplies the
// radius the game's setting asks for, 1 being the game's own.
void configure(const Configuration& configuration) noexcept;

// The game's amount that makes the plugin's shutter ratio its own times `scale`. The plugin gives its
// blur min(exposure x amount, 1), `exposure` being the frame rate times the shutter, so a scale on the
// amount alone did nothing while the product passed 1. None when the exposure is not a positive number.
inline std::optional<float> scaledAmount(float exposure, float gameAmount, float scale) noexcept
{
    if (!(exposure > 0) || !std::isfinite(exposure)) return std::nullopt;

    const float ratio = std::min(exposure * gameAmount, 1.0f) * scale;
    return std::min(ratio, 1.0f) / exposure;
}
}
