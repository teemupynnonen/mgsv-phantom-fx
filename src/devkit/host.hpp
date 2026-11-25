#pragma once

#include <cstdint>
#include <string_view>

// What the add-on that uses the devkit supplies: its name, and where its notifications go.
namespace devkit
{
// "PhantomFX", in the Lua chunk names the game's errors show. A literal that outlives the add-on.
void setName(const char* name) noexcept;
std::string_view name() noexcept;

// fxui::toast's tones, in its order.
enum class Tone : std::uint8_t { Dim, Ok, Warn, Bad };
using Toast = void (*)(Tone tone, std::string_view text, std::string_view detail);

void setToast(Toast toast) noexcept;
// Any thread may call it, as fxui::toast may be.
void toast(Tone tone, std::string_view text, std::string_view detail = {}) noexcept;
}
