#pragma once

#include "phantom_fx_settings.hpp"

#include <optional>
#include <string>

// Photo mode (F2): the game freezes through its own pause requests while a free camera flies through
// the frozen scene, with the UI hidden and the depth of field focused on the centre of the screen
// (focus.hpp). Hiding the UI (F3) also works on its own, at any time. docs/DEVELOPMENT.md, "Photo mode".
namespace phantom_fx::photo_mode
{
// The keys held this frame, as the free camera reads them.
struct Controls
{
    bool forward{}, back{}, left{}, right{}, up{}, down{}, fast{};
};

// A camera in world space: yaw 0 looks along +z, turning towards +x; the pitch is positive upwards.
// Radians; roll is not kept.
struct Pose
{
    float position[3]{};
    float yaw{}, pitch{};
};

// Hooks the camera selector and the game's raw input on the supported build, with MinHook
// initialised; returns what was done, for the log. Without the hooks photo mode is unavailable;
// hiding the UI does not need them.
std::string hook() noexcept;
// Applies at the game's next camera update. The free camera starts where the game's camera is.
void toggle() noexcept;
void toggleUi() noexcept;
// Photo mode's depth of field blur (F4): on by default; off, everything is sharp. Photo mode sets the
// game camera's lens, which the game's depth of field reads.
void toggleBlur() noexcept;
// The lens's f-number, from the settings.
void configure(const Configuration& configuration) noexcept;
void setControls(const Controls& controls) noexcept;
// In photo mode, or hidden with F3: the 2D pass does not draw into the scene.
bool uiHidden() noexcept;
// Unfreezes the game, for when the add-on unloads in photo mode.
void leave() noexcept;

// Automation (automation.hpp): photo mode at exactly `pose`, the mouse and keys ignored and the depth
// of field off, until released. Applies at the game's next camera update.
void hold(const Pose& pose) noexcept;
// From the held camera (or the game's) to `pose` in `updates` equal steps, one each camera update, then
// held there.
void move(const Pose& pose, int updates) noexcept;
void release() noexcept;
bool isHeld() noexcept;
// The camera the game's last camera update rendered from; empty before the first.
std::optional<Pose> lastCamera() noexcept;
}
