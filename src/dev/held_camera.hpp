#pragma once

#include "features/photo_mode/photo_mode.hpp"

#include <optional>

// The debug cases' camera (automation.hpp): photo mode at exactly a pose, the mouse and keys ignored and the depth of
// field off, until released.
namespace phantom_fx::held_camera
{
using photo_mode::Pose;

// Applies at the game's next camera update.
void hold(const Pose& pose) noexcept;
// From the held camera (or the game's) to `pose` in `updates` equal steps, one each camera update, then held there.
void move(const Pose& pose, int updates) noexcept;
void release() noexcept;
bool isHeld() noexcept;
// The camera the game's last camera update rendered from; empty before the first.
std::optional<Pose> lastCamera() noexcept;

// From photo mode's camera update: the held camera for this update, a moving one a step on, and the camera it used.
std::optional<Pose> next() noexcept;
void remember(const Pose& pose) noexcept;
}
