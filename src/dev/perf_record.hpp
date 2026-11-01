#pragma once

#include "performance.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>

// What frames cost, on file (docs/DEVELOPMENT.md, "Costs on file"), so a play session needs no one watching it.
// `perf/<time>.jsonl` beside the automation's commands takes one JSON object a second: the frames' times, each pass and
// each traced phase, the rays, the lights and the scene, with where the game was and what time it was there.
// capture() takes the same over a set number of frames into a file of its own, to compare one frozen frame under
// changes (tools/automation/perf.py).
namespace phantom_fx::perf_record
{
// What a kick traced.
struct Kick
{
    std::uint32_t width{}, height{};
    std::uint32_t instances{}, skins{}, builds{};
    std::uint64_t triangles{}, builtTriangles{};
};

void install(const std::filesystem::path& folder) noexcept;
// The game thread, at the main loop's frame start.
void onFrameStart() noexcept;

// Each present, with the newest frame the GPU finished since the last, if any.
void onPresent(const std::optional<performance::FrameCost>& cost) noexcept;
// The frame's lights: those the kick traces and those past the rays' cap.
void onLights(std::size_t traced, std::size_t overCap) noexcept;
void onKick(const Kick& kick) noexcept;
// A frame's traced phases in milliseconds, once the GPU is past them. The names are literals.
void onPhases(std::span<const std::pair<const char*, double>> phases) noexcept;
// A frame's GI_COUNTER_ values, once the GPU is past them.
void onCounters(std::span<const std::uint32_t> counters) noexcept;

// The next `frames` presents' costs into `file`.json. A capture asked for during another replaces it.
void capture(const std::filesystem::path& file, std::uint32_t frames) noexcept;
// Captures finished so far, and the line that says how the last went.
std::uint64_t capturesFinished() noexcept;
std::string lastCapture();
}
