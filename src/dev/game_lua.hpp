#pragma once

#include <optional>
#include <string>
#include <string_view>

// Lua in the game's own script state (Lua 5.1.5), run the way the game's own calls into it run: for
// the Developer page's tools and its console. docs/DEVELOPMENT.md, "Game Lua".
namespace phantom_fx::game_lua
{
struct Outcome
{
    bool ran{};       // To its end, without an error
    std::string text; // The lines it printed, then the values it returned (tab-separated) or its error
};

// The supported build with the expected engine code; otherwise nothing runs.
bool isSupported() noexcept;
// Runs `code` now: only on the game thread outside the game's own Lua calls, such as at the main
// loop's frame start. With `expression`, `code` is first tried as an expression and its value
// returned, as an interactive Lua does. Empty before the game's scripts start.
std::optional<Outcome> run(std::string_view code, std::string_view name, bool expression = false) noexcept;

// The console: `code` runs at the next frame start, and its outcome stays until the next run.
void submit(std::string code) noexcept;
struct Console
{
    bool waiting{}; // Submitted, not run yet
    std::optional<Outcome> last;
    int runs{}; // How many submissions ran, to tell one outcome from the next
};
Console console() noexcept;
// The game thread, at the main loop's frame start.
void onFrameStart() noexcept;
}
