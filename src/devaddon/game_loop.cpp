#include "devaddon/game_loop.hpp"

#include "devkit/automation.hpp"
#include "devkit/cutscenes.hpp"
#include "devkit/game.hpp"
#include "devkit/game_lua.hpp"
#include "devkit/travel.hpp"
#include "game_code.hpp"
#include "game_loop_code.hpp"
#include "log.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>

namespace devaddon
{
namespace
{
// The main loop's frame start, on the loop object (rcx). PhantomFX's game_loop.hpp has the loop's end too.
using phantom_fx::game_loop_code::FrameStartCode;
using phantom_fx::game_loop_code::FrameStartRva;

using FrameStart = void (*)(std::uint8_t* loop, void* argument);
using RegisterFrameStart = bool (*)(void (*)());
using UnregisterFrameStart = void (*)(void (*)());
FrameStart original{};
bool withPhantomFx{};
bool pending{};

void devkitFrameStart()
{
    devkit::travel::onFrameStart();
    devkit::cutscenes::onFrameStart();
    devkit::game_lua::onFrameStart();
    devkit::automation::onFrameStart();
}

void frameStart(std::uint8_t* loop, void* argument)
{
    devkitFrameStart();
    original(loop, argument);
}

HMODULE phantomFx() noexcept
{
    return GetModuleHandleW(L"PhantomFX.addon64");
}
}

std::string hook() noexcept
{
    // PhantomFX already hooks the loop: its callbacks run on the game thread without a second hook.
    if (const auto module = phantomFx())
    {
        const auto registerFrameStart = reinterpret_cast<RegisterFrameStart>(GetProcAddress(module, "PhantomFXRegisterFrameStart"));
        if (registerFrameStart && registerFrameStart(&devkitFrameStart))
        {
            withPhantomFx = true;
            return "Registered on PhantomFX's frame start.";
        }
    }

    if (!devkit::game::isSupportedBuild()) return "Unknown game build; the game loop is not hooked.";

    auto* base = phantom_fx::game_code::image();
    auto* start = base + FrameStartRva;
    const devkit::game::Expected expected{FrameStartRva, FrameStartCode};
    if (!devkit::game::matchesPastHooks({&expected, 1}))
        return "The game loop has unexpected code; the tools that run at the frame start are unavailable.";

    // An add-on loaded before this, an older PhantomFX among them, may already hook the frame start. Its
    // jump is all that is left of the first bytes; hooking on top calls through it.
    const bool onAnotherHook = devkit::game::hookJumpLength(start) != 0;
    const auto status = phantom_fx::game_code::hookAt(start, &frameStart, original);
    if (status != MH_OK) return std::string("Hooking the game loop failed (") + MH_StatusToString(status) + ").";

    if (!onAnotherHook) return "The game loop is hooked.";
    return phantomFx() ? "The game loop is hooked on top of this PhantomFX's own; it predates the devkit's frame start."
                       : "The game loop is hooked on top of another add-on's.";
}

void defer() noexcept
{
    pending = true;
}

void onPresent() noexcept
{
    if (!pending) return;
    pending = false;
    phantom_fx::log(hook());
}

void unhook() noexcept
{
    pending = false;
    if (withPhantomFx)
    {
        if (const auto module = phantomFx())
            if (const auto unregisterFrameStart = reinterpret_cast<UnregisterFrameStart>(GetProcAddress(module, "PhantomFXUnregisterFrameStart")))
                unregisterFrameStart(&devkitFrameStart);
        withPhantomFx = false;
        return;
    }

    if (!original) return;

    MH_RemoveHook(phantom_fx::game_code::image() + FrameStartRva);
    original = nullptr;
}
}
