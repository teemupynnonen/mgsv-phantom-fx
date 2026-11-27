#pragma once

#include <string>

// The game's main loop, for the devkit's tools: the frame start PhantomFX hooks for Reflex too, with
// only the devkit's calls on it. Another addon's own loop hook would have to call them instead.
namespace devaddon
{
// Hooks the loop on the supported build, with MinHook initialised; returns what was done, for the log.
std::string hook() noexcept;
// The add-on is loaded before PhantomFX: wait for the first present, when both are loaded, before choosing
// PhantomFX's frame start or a hook of the devkit's own.
void defer() noexcept;
// The first present: registers on PhantomFX's frame start when it is there, otherwise hooks the loop.
void onPresent() noexcept;
void unhook() noexcept;
}
