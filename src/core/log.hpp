#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#ifndef PHANTOM_FX_DIAGNOSTIC_LOG
#define PHANTOM_FX_DIAGNOSTIC_LOG 0
#endif

namespace phantom_fx
{
// The dev preset's build: the statistics logged every few seconds, and the measurements that only feed
// them, run. A release logs only what happens once: startup, sizes, failures.
inline constexpr bool DiagnosticLog = PHANTOM_FX_DIAGNOSTIC_LOG != 0;

// Opens the log at `file`, replacing a previous game session's.
void openLog(const std::filesystem::path& file);
// Appends a line to the log, from any thread.
void log(std::string_view message);
// The same, only the first time the message comes.
void logOnce(std::string_view message);
std::string hex(std::uint64_t value);
}
