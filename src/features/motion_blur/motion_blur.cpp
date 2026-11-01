#include "features/motion_blur/motion_blur.hpp"

#include "clock.hpp"
#include "device_check.hpp"
#include "game_code.hpp"
#include "game_memory.hpp"
#include "log.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

namespace phantom_fx::motion_blur
{
namespace
{
// GrPluginMotionBlur's PreExec and Exec (slots 6 and 8 of its vtable at 0x20F8830), which the
// recording threads call every frame with (plugin, pass context, view). With the game's motion
// blur on, PreExec asks for the velocity passes by setting bit 0 of the frame flags at
// [context + 0xD0], which MakeVelocityBuffer tests, and Exec draws the blur.
constexpr std::uintptr_t PreExecRva = 0x1E2D80, ExecRva = 0x1E2E20;
// cmp byte ptr [rcx + 0x68], 0; mov rax, [r8 + 0x570]
constexpr unsigned char PreExecStart[]{0x80, 0x79, 0x68, 0x00, 0x49, 0x8B, 0x80, 0x70, 0x05, 0x00, 0x00};
// mov [rsp + 8], rbx; mov [rsp + 0x10], rsi; push rdi; sub rsp, 0x20
constexpr unsigned char ExecStart[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20};
constexpr std::size_t FrameFlags = 0xD0;
constexpr std::uint64_t VelocityPasses = 1;
// Motion blur on, its minimum shutter and its amount, as PreExec and Exec read them (docs/DEVELOPMENT.md,
// "Engine addresses").
constexpr std::uintptr_t MotionBlurOnRva = 0x2B75AC4, MinimumShutterRva = 0x2B75AC8, AmountRva = 0x2B75ACC;
// What Exec takes its shutter ratio from besides the amount: the view's camera, with its shutter and
// frame time, and the plugin's own shutter, used in the camera's place when its flag is set. With bit 0
// of the plugin's flags, Exec takes 60 frames a second in place of the frame time.
constexpr std::size_t ViewCamera = 0x570, CameraShutter = 0x128, CameraFrameTime = 0x130;
constexpr std::size_t PluginHasShutter = 0x68, PluginShutter = 0x6C, PluginFlags = 0x78;
constexpr float FixedFrameRate = 60;

using PluginStep = void (*)(void* plugin, std::uint8_t* context, void* view);
PluginStep originalPreExec{}, originalExec{};
std::atomic<bool> velocityOnly{}, extraVelocityPasses{}, enabled{true}, frameGeneration{};
std::atomic<float> strength{1};
std::atomic<LONGLONG> lastStrengthLog{};

// The game's own amount, relearned when it writes it, so the strength multiplies the game's radius, and
// the amount last written. The recording threads read, work out and write the amount one at a time: in
// between, one thread's write would look like the game's own to another.
std::mutex amountMutex;
float gameAmount{1};
float writtenAmount{std::numeric_limits<float>::quiet_NaN()};

// The player's motion blur setting, as PreExec reads it.
bool gameWantsBlur() noexcept
{
    const auto on = game_memory::read<std::uint32_t>(reinterpret_cast<std::uintptr_t>(game_code::image()) + MotionBlurOnRva);

    return on && *on != 0;
}

// The frame rate times the shutter, as Exec takes them for `view`: its shutter ratio per unit of the
// amount. PreExec has just read the same camera.
float exposureOf(const std::uint8_t* plugin, const std::uint8_t* view, float minimumShutter) noexcept
{
    const auto* camera = *reinterpret_cast<const std::uint8_t* const*>(view + ViewCamera);
    const float cameraShutter = *reinterpret_cast<const float*>(camera + CameraShutter);
    const double frameTime = *reinterpret_cast<const double*>(camera + CameraFrameTime);
    const float pluginShutter = *reinterpret_cast<const float*>(plugin + PluginShutter);

    const float shutter = std::max(plugin[PluginHasShutter] ? pluginShutter : cameraShutter, minimumShutter);
    const float rate = (plugin[PluginFlags] & 1) ? FixedFrameRate : static_cast<float>(1.0 / frameTime);

    return rate * shutter;
}

// Every ten seconds: what the blur was given, so a strength that seems to do nothing can be checked.
// Under amountMutex.
void logStrength(float exposure, float amount) noexcept
{
    const auto now = clock::ticksNow();
    if (clock::millisecondsOf(now - lastStrengthLog.load(std::memory_order_relaxed)) < 10000) return;

    lastStrengthLog = now;
    const auto number = [](float value) { return std::to_string(value); };
    log("Motion blur: frame rate x shutter " + number(exposure) + ", the game's amount " + number(gameAmount)
        + ", its shutter ratio " + number(std::min(exposure * gameAmount, 1.0f)) + ", given "
        + number(std::min(exposure * amount, 1.0f)) + " (strength " + number(strength.load(std::memory_order_relaxed))
        + (frameGeneration.load(std::memory_order_relaxed) ? ", halved for frame generation)." : ")."));
}

// The amount that gives Exec's shutter ratio the game's own times the strength, halved while frame
// generation runs, written after PreExec has read it, so this frame's blur draws use it. The toggle is
// the add-on's own, in exec().
void applyStrength(const std::uint8_t* plugin, const std::uint8_t* view) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    std::lock_guard lock(amountMutex);
    const auto base = reinterpret_cast<std::uintptr_t>(game_code::image());
    const auto amount = game_memory::read<float>(base + AmountRva);
    const auto minimumShutter = game_memory::read<float>(base + MinimumShutterRva);
    if (!amount || !minimumShutter) return;

    if (!(std::fabs(*amount - writtenAmount) <= 1e-4f)) gameAmount = *amount;

    const float exposure = exposureOf(plugin, view, *minimumShutter);
    const float scale = strength.load(std::memory_order_relaxed) * (frameGeneration.load(std::memory_order_relaxed) ? 0.5f : 1.0f);
    const auto wanted = scaledAmount(exposure, gameAmount, scale);
    if (!wanted) return;

    if (!(std::fabs(*wanted - *amount) <= 1e-4f)) game_memory::write(base + AmountRva, *wanted);
    writtenAmount = *wanted;

    if constexpr (DiagnosticLog) logStrength(exposure, *wanted);
}

void preExec(void* plugin, std::uint8_t* context, void* view)
{
    originalPreExec(plugin, context, view);

    auto* flags = *reinterpret_cast<std::uint64_t**>(context + FrameFlags);
    if ((velocityOnly.load(std::memory_order_relaxed) || extraVelocityPasses.load(std::memory_order_relaxed)) && flags) *flags |= VelocityPasses;

    applyStrength(static_cast<const std::uint8_t*>(plugin), static_cast<const std::uint8_t*>(view));
}

void exec(void* plugin, std::uint8_t* context, void* view)
{
    // The bit may be the add-on's, forced on for the upscaler's motion vectors; the blur still follows
    // the add-on's toggle and the player's setting.
    if (enabled.load(std::memory_order_relaxed) && (!velocityOnly.load(std::memory_order_relaxed) || gameWantsBlur()))
        originalExec(plugin, context, view);
}
}

std::string hook() noexcept
{
    if (!isSupportedGameBuild()) return "Unknown game build; motion blur is left to the game.";

    auto* base = game_code::image();
    auto* preExecTarget = base + PreExecRva;
    auto* execTarget = base + ExecRva;
    if (std::memcmp(preExecTarget, PreExecStart, sizeof(PreExecStart)) || std::memcmp(execTarget, ExecStart, sizeof(ExecStart)))
        return "The motion blur plugin has unexpected code; motion blur is left to the game.";

    auto status = game_code::hookAt(preExecTarget, &preExec, originalPreExec);
    if (status == MH_OK)
    {
        status = game_code::hookAt(execTarget, &exec, originalExec);
        if (status != MH_OK) MH_RemoveHook(preExecTarget);
    }

    if (status != MH_OK)
        return std::string("Hooking the motion blur plugin failed (") + MH_StatusToString(status) + "); motion blur is left to the game.";

    return "The motion blur plugin is hooked: with DLSS on, the engine draws its velocity passes every frame, and its blur as the game's setting says.";
}

void setVelocityOnly(bool on) noexcept
{
    velocityOnly = on;
}

void wantVelocityPasses(bool on) noexcept
{
    extraVelocityPasses = on;
}

void setFrameGeneration(bool on) noexcept
{
    frameGeneration = on;
}

void configure(const Configuration& configuration) noexcept
{
    enabled = configuration.settings.motionBlur.on;
    strength = std::clamp(configuration.settings.motionBlur.strength, 0.0f, 2.0f);
}
}
