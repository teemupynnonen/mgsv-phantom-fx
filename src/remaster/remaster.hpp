#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace phantom_fx
{
struct Configuration;
}

// The remastering tool (README.md): what the rest of the add-on knows of it. The rules saved in
// materials.json change the game's materials as they are made, in every build; only a dev build has the tool
// that writes them.
namespace phantom_fx::remaster
{
// With MinHook initialised: reads the rules and hooks the game's materials. Returns what was done, for
// the log.
std::string hook() noexcept;

// Takes the rules off the game's materials while the enhancements switch (F1) is off.
void configure(const Configuration& configuration) noexcept;
void present() noexcept;

// A self-lit material's values as a model file gives them, as the rules make them: the ray-traced scene
// reads the file, not the game's materials. A file already read keeps what it had, so a change in the
// tool reaches the rays at the next start.
void tuneFileSelfLight(std::uint64_t material, std::array<float, 4>& color, std::array<float, 4>& intensity);

// The tool's item on the Developer page; only a dev build has it.
void drawTool();
}
