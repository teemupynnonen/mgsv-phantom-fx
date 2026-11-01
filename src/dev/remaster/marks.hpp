#pragma once

#include "remaster/rules.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The tool's marks: what was on screen at a place, kept for working down later (README.md). A dev build's
// scratch beside the add-on, not the repository's.
namespace phantom_fx::remaster
{
enum class MaterialKind : std::uint8_t
{
    Other,
    SelfLit,
    Glass
};

// A material seen at a mark. Rows of the game's pool change with each load, so `fingerprint` says which
// material this was.
struct MarkedMaterial
{
    std::uint64_t name{};
    std::uint64_t fingerprint{};
    MaterialKind kind{};
    float brightness{};
    std::int32_t uses{};

    bool operator==(const MarkedMaterial&) const = default;
};

struct Mark
{
    std::uint16_t location{}, mission{};
    std::array<float, 3> position{};
    float rotation{};
    float clock{}; // Seconds since midnight
    std::vector<MarkedMaterial> materials;

    bool operator==(const Mark&) const = default;
};

// The marks, or none and why.
struct ParsedMarks
{
    std::vector<Mark> marks;
    std::string error;
};

ParsedMarks parseMarks(std::string_view text);
std::string writeMarks(const std::vector<Mark>& marks);

// The map and position, as "Mother Base, at 8, 1, -47". The game has no name for a place.
std::string placeText(const Mark& mark);
// The time of day, as "22:00".
std::string timeText(float clock);

// The material with the rule on it taken off, so tuning it does not make it another material.
std::uint64_t fingerprintOf(const PackedMaterial& material, std::uint64_t name, std::span<const Rule> rules);
}
