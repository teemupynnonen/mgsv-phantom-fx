#pragma once

#include "remaster/rules.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// The game's materials as it keeps them (docs/DEVELOPMENT.md, "Materials"): a pool in main memory that each draw
// copies its constants from, so a write here shows at the next draw.
namespace phantom_fx::remaster::game_materials
{
struct Material
{
    std::uint32_t index{}; // Its row in the pool
    std::uint64_t name{};
    std::uint64_t technique{};
    std::int32_t uses{};
    PackedMaterial packed; // The game's own memory
};

// The material at a row of the pool, whether or not anything uses it; nothing where the row has no
// model's kind of technique.
std::optional<Material> materialAt(std::uint32_t row) noexcept;

// Every material in use.
void forEachMaterial(const std::function<void(const Material&)>& visit);

using NewMaterial = void (*)(const Material& material) noexcept;

// With MinHook initialised: `onNew` meets each material just before the game merges it with an equal
// one, so what it changes decides the merge. On the game's loading threads. Returns MinHook's failure,
// or why not, empty when hooked.
std::string hookNewMaterials(NewMaterial onNew) noexcept;
}
