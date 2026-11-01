#pragma once

#include <cstdint>
#include <filesystem>

namespace phantom_fx::trace
{
class Trace;
}

// A debug case's record of every pixel, a trace on the ray-traced scene: what the rays meet there (inspect.hlsl), for
// tools/automation/case.py to compare with the game's own answers.
namespace phantom_fx::inspect_trace
{
trace::Trace& trace() noexcept;

// Records the next frame that can take it, into `file` with the extensions .bin and .tsv.
void request(const std::filesystem::path& file);
// The records finished so far, written or not: a failure is in the log.
std::uint64_t finished() noexcept;
}
