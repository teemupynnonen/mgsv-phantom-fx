#include "game_loop.hpp"

#include "character.hpp"
#include "dev/dev.hpp"
#include "dev_tools.hpp"
#include "device_check.hpp"
#include "features/frame_generation/frame_generation.hpp"
#include "game_code.hpp"
#include "weather.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>

namespace phantom_fx::game_loop
{
namespace
{
// The main loop's frame start and end, both on the loop object (rcx). The start increments the frame
// count at loop + FrameCountOffset and copies it to a global.
constexpr std::uintptr_t FrameStartRva = 0x7F840, FrameEndRva = 0x7FDB0;
constexpr std::size_t FrameCountOffset = 0x150;
// push rbx; push rsi; push rdi; sub rsp, 0x60; mov qword ptr [rsp + 0x20], -2
constexpr unsigned char FrameStartCode[]{0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF};
// sub rsp, 0x28; mov dword ptr [rcx + 0xA8], 0
constexpr unsigned char FrameEndCode[]{0x48, 0x83, 0xEC, 0x28, 0xC7, 0x81, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Neither caller uses a result.
using FrameStart = void (*)(std::uint8_t* loop, void* argument);
using FrameEnd = void (*)(std::uint8_t* loop);
FrameStart originalFrameStart{};
FrameEnd originalFrameEnd{};
std::atomic<std::uint32_t> current{};

#if PHANTOM_FX_DEV_TOOLS
std::mutex callbacksMutex;
std::array<FrameStartCallback, 8> frameStartCallbacks{};
// Held shared while copied callbacks run, and taken alone by unregistration: it returns only once a callback that
// was copied before it was cleared has finished. A callback must not register or unregister itself.
// Not needed in theory: ReShade unloads add-ons only from D3D12Device's destructor, once the last device is gone, which
// in this game is after the main loop has stopped. Kept because the ordering should not rest on that.
std::shared_mutex dispatchMutex;
#endif

std::uint32_t countOf(const std::uint8_t* loop)
{
    return *reinterpret_cast<const std::uint32_t*>(loop + FrameCountOffset);
}

void frameStart(std::uint8_t* loop, void* argument)
{
    // Reflex sleeps before the frame's time step, so the step includes the sleep.
    const auto frame = countOf(loop) + 1;
    frame_generation::beginSimulation(frame);
    weather::onFrameStart();
    character::onFrameStart();
    if constexpr (DevTools) dev::onFrameStart();
#if PHANTOM_FX_DEV_TOOLS
    {
        // The shared lock first: an unregistration that clears a slot afterwards waits for this dispatch, and one that
        // cleared it before is seen in the copy. Copying first left a gap where a callback could run after its
        // unregistration had returned.
        const std::shared_lock dispatch(dispatchMutex);
        std::array<FrameStartCallback, 8> callbacks;
        {
            std::scoped_lock lock(callbacksMutex);
            callbacks = frameStartCallbacks;
        }
        for (const auto callback : callbacks)
            if (callback) callback();
    }
#endif
    originalFrameStart(loop, argument);
    current = frame;
}

// Before the wait for the next frame, which is not simulation. The loop has recorded the frame's
// scene by now; the render thread executes and presents it during the next frame.
void frameEnd(std::uint8_t* loop)
{
    const auto frame = countOf(loop);
    frame_generation::endSimulation(frame);
    frame_generation::beginRender(frame);
    originalFrameEnd(loop);
}
}

std::string hook() noexcept
{
    if (!isSupportedGameBuild()) return "Unknown game build; the game loop is not hooked.";

    auto* base = game_code::image();
    if (std::memcmp(base + FrameStartRva, FrameStartCode, sizeof(FrameStartCode)) || std::memcmp(base + FrameEndRva, FrameEndCode, sizeof(FrameEndCode)))
        return "The game loop has unexpected code; Reflex marks frames at the present and the weather and character menus are unavailable.";

    auto status = game_code::hookAt(base + FrameStartRva, &frameStart, originalFrameStart);
    if (status == MH_OK)
    {
        status = game_code::hookAt(base + FrameEndRva, &frameEnd, originalFrameEnd);
        if (status != MH_OK)
        {
            MH_RemoveHook(base + FrameStartRva);
            // The trampoline outlives the removed hook: registration must not think a dispatcher is running.
            originalFrameStart = nullptr;
        }
    }

    if (status != MH_OK)
        return std::string("Hooking the game loop failed (") + MH_StatusToString(status)
            + "); Reflex marks frames at the present and the weather and character menus are unavailable.";

    return "The game loop is hooked, for Reflex and the weather and character menus.";
}

std::uint32_t currentFrame() noexcept
{
    return current;
}

#if PHANTOM_FX_DEV_TOOLS
bool registerFrameStart(FrameStartCallback callback) noexcept
{
    // Without the hook there is no frame start to call it from: the caller must fall back to its own.
    if (!callback || !originalFrameStart) return false;

    std::scoped_lock lock(callbacksMutex);
    for (const auto registered : frameStartCallbacks)
        if (registered == callback) return true;
    for (auto& slot : frameStartCallbacks)
    {
        if (slot) continue;

        slot = callback;
        return true;
    }

    return false;
}

void unregisterFrameStart(FrameStartCallback callback) noexcept
{
    {
        std::scoped_lock lock(callbacksMutex);
        for (auto& slot : frameStartCallbacks)
            if (slot == callback) slot = nullptr;
    }

    // A frame start may have copied the callback before it was cleared: this returns when that call has finished,
    // so the module can unload.
    const std::unique_lock dispatch(dispatchMutex);
}
#endif
}

#if PHANTOM_FX_DEV_TOOLS
// The devkit add-on's way onto this add-on's frame start, without a second MinHook hook of its own. Not in a release
// add-on: with no export, DevKit hooks the frame start itself.
extern "C" __declspec(dllexport) bool PhantomFXRegisterFrameStart(void (*callback)()) noexcept
{
    return phantom_fx::game_loop::registerFrameStart(callback);
}

extern "C" __declspec(dllexport) void PhantomFXUnregisterFrameStart(void (*callback)()) noexcept
{
    phantom_fx::game_loop::unregisterFrameStart(callback);
}
#endif
