#pragma once

#include <atomic>

// Effects that need the engine's velocity passes (SSGI's temporal accumulation) ask for them here, so
// none depends on the motion blur feature, whose hook is what sets the engine's flag from this request.
namespace phantom_fx::velocity_request
{
inline std::atomic<bool> wanted{};

// While on, the engine draws its velocity passes every frame, whatever the upscaler asks. The game's
// blur still follows its own setting and the add-on's toggle.
inline void want(bool on) noexcept
{
    wanted.store(on, std::memory_order_relaxed);
}

inline bool isWanted() noexcept
{
    return wanted.load(std::memory_order_relaxed);
}
}
