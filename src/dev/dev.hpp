#pragma once

#include <filesystem>

struct ID3D11DeviceContext;
struct ID3D11Resource;

// All the add-on calls of the development tools (README.md here). Only a dev build has them, so each call stands behind
// `if constexpr (DevTools)` (dev_tools.hpp), which leaves a release build nothing to link.
namespace phantom_fx::dev
{
void install(const std::filesystem::path& addonDirectory) noexcept;
// The game thread, at the main loop's frame start.
void onFrameStart() noexcept;
// The overlay's Developer page.
void drawPage();
// While DevKit's frame log records a frame, the folder the passes save what they draw into; empty otherwise.
std::filesystem::path recordingDirectory() noexcept;
}
