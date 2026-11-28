#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The overlay's control for the game's weather and time of day, to see the sky in every condition. It
// calls what the game's WeatherManager script functions call, on the game thread at the main loop's
// frame start (game_loop.hpp). docs/DEVELOPMENT.md, "Weather and time".
namespace phantom_fx::weather
{
// The game's weather types (TppDefine.WEATHER).
enum class Type : std::uint8_t { Sunny, Cloudy, Rainy, Sandstorm, Foggy, Count };

// The game's state at the last frame start.
struct State
{
    bool available{}; // In a mission or free roam, where the game runs its weather
    float clock{};    // Seconds since midnight
    bool clockStopped{};
    std::array<bool, static_cast<std::size_t>(Type::Count)> typeAvailable{}; // The location has the weather's settings
    std::optional<Type> requested; // The strongest request: the weather the sky shows or moves to
    std::optional<Type> forced;    // Forced from the menu, even by an earlier session through a checkpoint
    float fogDensity{};            // The forced fog's, 0 to 1
};

// The supported build with the expected engine code; otherwise the menu cannot change anything.
bool supported() noexcept;
State state() noexcept;

// Whether the game's time is stopped: a pause menu, photo mode, a frozen frame. What moves with game time (the sea, the
// clouds' drift) stops with it.
bool isGamePaused() noexcept;
// These apply at the next frame start.
void setClock(float seconds) noexcept;
void stopClock(bool stopped) noexcept;
// Overrides the game's own weather, as its mission scripts do, until released; at once, without the
// game's transition. The fog density only counts for Foggy.
void force(Type type, float fogDensity) noexcept;
void release() noexcept;
std::string_view label(Type type) noexcept;
// The game thread, at the main loop's frame start: applies what the menu asked for and records the state.
void onFrameStart() noexcept;
}
