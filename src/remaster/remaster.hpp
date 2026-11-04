#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace phantom_fx
{
struct Configuration;
}

// The remastering rules (README.md): what the rest of the add-on knows of them. The rules saved in materials.json change
// the game's materials as they are made, in every build; only a dev build has the tool that writes them (dev/remaster).
namespace phantom_fx::remaster
{
// With MinHook initialised: reads the rules and hooks the game's materials. Returns what was done, for
// the log.
std::string hook() noexcept;

// Takes the rules off the game's materials while the enhancements switch (F1) is off.
void configure(const Configuration& configuration) noexcept;

// A self-lit material's values as a model file gives them, as the rules make them: the ray-traced scene
// reads the file, not the game's materials.
void tuneFileSelfLight(std::uint64_t material, std::array<float, 4>& color, std::array<float, 4>& intensity);

// Changes whenever the rules do, so what reads them through tuneFileSelfLight knows to read them again.
std::uint64_t rulesVersion() noexcept;

// What the rules make of the light of the game's lamp at `at` (world space), its colour `color` as its light draw has it:
// a factor, 1 for a lamp they leave alone and while the enhancements switch (F1) is off. On the game's render threads.
float lampBrightness(const std::array<float, 3>& at, const std::array<float, 3>& color) noexcept;
}
