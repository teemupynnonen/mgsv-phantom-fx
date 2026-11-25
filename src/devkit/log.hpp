#pragma once

#include <string_view>

// The devkit's messages go to the add-on that uses it: its log, its rules (log.hpp in PhantomFX).
namespace devkit
{
using Logger = void (*)(std::string_view message);

// Set before install() is called; the add-on's own log.
void setLogger(Logger logger) noexcept;
// Appends a line, from any thread. Does nothing until a logger is set.
void log(std::string_view message) noexcept;
}
