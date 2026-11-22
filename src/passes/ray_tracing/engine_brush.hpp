#pragma once

#include "passes/ray_tracing/scene_description.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>

// The game's object brush (docs/DEVELOPMENT.md, "Scene objects"): the vegetation and scattered props it
// places block by block over the terrain (bushes, grass, rocks), read from the loaded blocks' own
// placement records, each with the model its plugin draws.
namespace phantom_fx::engine_brush
{
// Hooks the brushes' and the blocks' constructors and destructors, with MinHook initialised and
// `base` the game's image; what was done, for the log.
std::string hook(std::uintptr_t base) noexcept;

// The placements in the loaded blocks, a plugin's in a block at a time: its model resource and their
// transforms, and whether its plugin is the fluttering grass (which the game sways in the wind). A block
// is read once, when first seen. On the thread taking the engine scene's snapshot.
void collect(const std::function<void(std::uintptr_t resource, std::span<const scene_description::Transform> transforms, bool grass)>& place);

// For the log.
std::string status();
}
