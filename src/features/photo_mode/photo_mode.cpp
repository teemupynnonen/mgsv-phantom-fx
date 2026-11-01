#include "features/photo_mode/photo_mode.hpp"

#include "clock.hpp"
#include "dev/game_lua.hpp"
#include "dev/held_camera.hpp"
#include "dev_tools.hpp"
#include "device_check.hpp"
#include "features/photo_mode/focus.hpp"
#include "fxui/fxui.hpp"
#include "game_code.hpp"
#include "log.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <utility>

namespace phantom_fx::photo_mode
{
namespace
{
// The main camera selector's update, on the game thread every frame (from 0x438470): of the cameras
// at [selector + 0x90] (count at + 0x88), the first with its active byte set becomes the render view's
// GrCamera ([view + 0x570]): world and view matrices from its rotation (a quaternion x, y, z, w) and
// position, lens and clip planes from the rest. It also places the audio listener. Culling, lighting
// and the scene constants all follow GrCamera.
constexpr std::uintptr_t SelectorUpdateRva = 0x437B60;
// mov rax, rsp; push rbp; push r12; push r13; push r14; push r15; lea rbp, [rax - 0x5F]
constexpr unsigned char SelectorUpdateStart[]{0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1};
constexpr std::size_t CameraCount = 0x88, Cameras = 0x90, CameraActive = 0xE0, CameraTransform = 0x120;
// The camera's lens, which the depth of field plugin (0x1DD1F0) reads through GrCamera: the focus
// distance in metres and the aperture as an f-number, from f/100 up no blur at all. The plugin
// raises the aperture to a minimum for the focal length (f/3 at 21 mm) unless the camera's byte
// +0xEA is set.
constexpr std::size_t CameraFocusDistance = 0x38, CameraAperture = 0x44, CameraAnyAperture = 0xEA;

// The window's raw input, from its WM_INPUT handler: reads the RAWINPUT and hands mouse and keyboard
// data to the game's devices.
constexpr std::uintptr_t RawInputRva = 0x1A45540;
// mov [rsp + 8], rbx; mov [rsp + 0x20], rsi; push rdi; sub rsp, 0x250
constexpr unsigned char RawInputStart[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xEC, 0x50, 0x02, 0x00, 0x00};
constexpr USHORT ButtonReleases = RI_MOUSE_BUTTON_1_UP | RI_MOUSE_BUTTON_2_UP | RI_MOUSE_BUTTON_3_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;

// The pause manager, a global: up to 16 requests, each a dword of pause bits with bit 31 set while
// active. Each frame start ORs the active ones into [manager + 8]; any bit stops game time (0x816A0)
// and systems test their own bits. Create (vtable slot 3) takes (manager, bits, name hash, name) and
// returns the request inactive; the game sets and clears bit 31 itself.
constexpr std::uintptr_t PauseManagerRva = 0x2B65D40, PauseVtableRva = 0x20C56C0, CreatePauseRva = 0xF1C80;
// push rsi; push rdi; push r14; sub rsp, 0x30
constexpr unsigned char CreatePauseStart[]{0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30};
// Most of the game's own requests ask for every bit but bit 0; under them the camera selector and
// rendering go on.
constexpr std::uint32_t PhotoPauses = 0x7FFFFFFE, Active = 0x80000000;
constexpr std::uint32_t PauseName = 0x4F544850; // A hash none of the game's requests use

// The weather manager's update (0x537DD0) tests two pause bits: bit 9 holds the weather and time of
// day where they are, bit 8 skips the rest of the update. Either keeps the weather menu's changes
// from showing, so while photo mode pauses, the update ignores both. Its clock still stands: game
// time steps by zero under any pause bit.
constexpr std::uintptr_t WeatherPauseTestsRva = 0x537FA3;
// mov ebx, [rax + 8]; shr ebx, 9; and bl, 1; jne; cmp byte ptr [rbp + 0xEC8], r13b; je;
// xorps xmm7, xmm7; call 0xF1D80; mov ecx, [rax + 8]; shr ecx, 8; test cl, 1; je +0x0C
constexpr unsigned char WeatherPauseTests[]{0x8B, 0x58, 0x08, 0xC1, 0xEB, 0x09, 0x80, 0xE3, 0x01, 0x75, 0x09, 0x44, 0x38, 0xAD, 0xC8,
    0x0E, 0x00, 0x00, 0x74, 0x03, 0x0F, 0x57, 0xFF, 0xE8, 0xC1, 0x9D, 0xBB, 0xFF, 0x8B, 0x48, 0x08, 0xC1, 0xE9, 0x08, 0xF6, 0xC1, 0x01,
    0x74, 0x0C};
// In photo mode the bit 9 test becomes `and bl, 0` and the jump past the bit 8 test a `jmp`.
constexpr std::size_t HoldMask = 8, SkipJump = 0x25;
constexpr unsigned char TestBit = 0x01, NoBit = 0x00, JumpIfZero = 0x74, Jump = 0xEB;

// Metres per second, radians per mouse count.
constexpr float Speed = 5, FastSpeed = 25, SlowSpeed = 0.5f, MouseSensitivity = 0.001f;
// Just short of straight up or down, where the yaw would be lost.
constexpr float MaxPitch = 1.55f;
// A hitch does not throw the camera.
constexpr double MaxStep = 0.1;
// With the blur off, f/64 keeps everything sharp. The depth of field plugin skips f/100 and up.
constexpr float SharpAperture = 64.0f;
// Seconds the autofocus takes to settle on a new distance, about.
constexpr float FocusTime = 0.2f;

using SelectorUpdate = void (*)(std::uint8_t* selector);
using RawInput = bool (*)(void* devices, HRAWINPUT input, HWND window);
using CreatePause = std::uint32_t* (*)(void* manager, std::uint32_t pauses, std::uint32_t nameHash, const char* name);
SelectorUpdate originalSelectorUpdate{};
RawInput originalRawInput{};
CreatePause createPause{};
void* pauseManager{};
std::uint8_t* weatherPauseTests{}; // Null when the update's code is unexpected

// Fox Engine is right-handed with y up, and a camera looks along its local +z. Yaw 0 looks along
// +z, and the pitch is positive upwards.
struct FreeCamera
{
    float position[3]{};
    float yaw{}, pitch{};
    float focusDistance{};
};

// What the selector's update reads from the active camera that photo mode sets.
struct CameraState
{
    float transform[8]{}; // Rotation, position
    float focusDistance{}, aperture{};
    std::uint8_t anyAperture{};
};

std::atomic<bool> installed{}, requested{}, flying{}, hideUi{}, blur{true};
std::atomic<float> fNumber{1.4f};
std::atomic<std::uint8_t> heldKeys{};
std::atomic<int> mouseX{}, mouseY{};
std::atomic<std::uint32_t*> pauseRequest{};
std::atomic<bool> teleportRequested{};

// Game thread only.
FreeCamera freeCamera;
LONGLONG lastStep{};

// Where the last Enter sends the player: the free camera's pose, taken at the selector update it arrives
// on, and the warp run at a frame start after leaving photo mode has let the game's time run. The dev
// build only.
Pose warp;
bool warpPending{};

enum Key : std::uint8_t { Forward = 1, Back = 2, Left = 4, Right = 8, Up = 16, Down = 32, Fast = 64, Slow = 128 };

std::uint8_t* activeCamera(std::uint8_t* selector)
{
    const auto count = *reinterpret_cast<const std::uint32_t*>(selector + CameraCount);
    auto* const* cameras = *reinterpret_cast<std::uint8_t* const* const*>(selector + Cameras);
    for (std::uint32_t i = 0; cameras && i < count; ++i)
        if (cameras[i] && cameras[i][CameraActive]) return cameras[i];

    return nullptr;
}

CameraState stateOf(const std::uint8_t* camera)
{
    CameraState state;
    std::memcpy(state.transform, camera + CameraTransform, sizeof(state.transform));
    std::memcpy(&state.focusDistance, camera + CameraFocusDistance, sizeof(state.focusDistance));
    std::memcpy(&state.aperture, camera + CameraAperture, sizeof(state.aperture));
    state.anyAperture = camera[CameraAnyAperture];

    return state;
}

void apply(std::uint8_t* camera, const CameraState& state)
{
    std::memcpy(camera + CameraTransform, state.transform, sizeof(state.transform));
    std::memcpy(camera + CameraFocusDistance, &state.focusDistance, sizeof(state.focusDistance));
    std::memcpy(camera + CameraAperture, &state.aperture, sizeof(state.aperture));
    camera[CameraAnyAperture] = state.anyAperture;
}

FreeCamera fromGame(const CameraState& game)
{
    const float* transform = game.transform;
    const float x = transform[0], y = transform[1], z = transform[2], w = transform[3];
    // The rotated +z axis; roll is dropped.
    const float forward[3]{2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};

    return {{transform[4], transform[5], transform[6]}, std::atan2(forward[0], forward[2]),
        std::clamp(std::asin(std::clamp(forward[1], -1.0f, 1.0f)), -MaxPitch, MaxPitch), game.focusDistance};
}

void place(FreeCamera& camera, const Pose& pose)
{
    std::memcpy(camera.position, pose.position, sizeof(camera.position));
    camera.yaw = pose.yaw;
    camera.pitch = std::clamp(pose.pitch, -MaxPitch, MaxPitch);
}

[[maybe_unused]] Pose poseOf(const FreeCamera& camera)
{
    Pose pose;
    std::memcpy(pose.position, camera.position, sizeof(pose.position));
    pose.yaw = camera.yaw;
    pose.pitch = camera.pitch;

    return pose;
}


// The rotation is the yaw about +y after the pitch about +x; looking up is a negative turn about +x.
CameraState photoState(const FreeCamera& camera, const CameraState& game, bool isSharp)
{
    const float sy = std::sin(camera.yaw / 2), cy = std::cos(camera.yaw / 2);
    const float sp = std::sin(-camera.pitch / 2), cp = std::cos(-camera.pitch / 2);

    auto state = game;
    const float rotation[4]{cy * sp, sy * cp, -sy * sp, cy * cp};
    std::memcpy(state.transform, rotation, sizeof(rotation));
    std::memcpy(state.transform + 4, camera.position, sizeof(camera.position));
    state.focusDistance = camera.focusDistance;
    state.aperture = blur && !isSharp ? fNumber.load() : SharpAperture;
    state.anyAperture = 1;

    return state;
}

void fly(FreeCamera& camera, std::uint8_t keys, int turnX, int turnY, float seconds)
{
    // Moving the mouse right turns right, which is towards -x when looking along +z.
    camera.yaw -= static_cast<float>(turnX) * MouseSensitivity;
    camera.pitch = std::clamp(camera.pitch - static_cast<float>(turnY) * MouseSensitivity, -MaxPitch, MaxPitch);

    const auto axis = [keys](Key positive, Key negative) { return static_cast<float>((keys & positive) != 0) - static_cast<float>((keys & negative) != 0); };
    const float ahead = axis(Forward, Back), aside = axis(Right, Left), rise = axis(Up, Down);
    const float speed = keys & Slow ? SlowSpeed : keys & Fast ? FastSpeed : Speed;
    const float distance = speed * seconds;
    const float forward[3]{std::cos(camera.pitch) * std::sin(camera.yaw), std::sin(camera.pitch), std::cos(camera.pitch) * std::cos(camera.yaw)};
    const float right[3]{-std::cos(camera.yaw), 0, std::sin(camera.yaw)};
    for (int i = 0; i < 3; ++i) camera.position[i] += distance * (ahead * forward[i] + aside * right[i]);

    camera.position[1] += distance * rise;
}

void letWeatherRun(bool run)
{
    auto* const code = weatherPauseTests;
    if (!code) return;

    DWORD protection{};
    if (!VirtualProtect(code, sizeof(WeatherPauseTests), PAGE_EXECUTE_READWRITE, &protection)) return;

    code[HoldMask] = run ? NoBit : TestBit;
    code[SkipJump] = run ? Jump : JumpIfZero;
    VirtualProtect(code, sizeof(WeatherPauseTests), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(WeatherPauseTests));
}

void pause(bool on)
{
    auto* request = pauseRequest.load();
    if (on && !request)
    {
        // Made once and kept, so the game's 16 requests are not used up.
        request = createPause(pauseManager, PhotoPauses, PauseName, "PhotoMode");
        pauseRequest = request;
        if (!request) log("The game has no free pause request; photo mode does not freeze it.");
    }

    if (!request) return;

    std::atomic_ref<std::uint32_t> flags(*request);
    if (on) flags.fetch_or(Active);
    else flags.fetch_and(~Active);

    letWeatherRun(on);
}

double secondsSince(LONGLONG& last)
{
    const auto now = clock::ticksNow();
    const double seconds = clock::millisecondsOf(now - last) / 1000;
    last = now;

    return std::min(seconds, MaxStep);
}

void selectorUpdate(std::uint8_t* selector)
{
    auto* camera = activeCamera(selector);
    std::optional<Pose> heldCamera;
    if constexpr (DevTools) heldCamera = held_camera::next();

    // Enter asks for the player to go where the free camera is. The warp waits for the game's time, so
    // the request also leaves photo mode: the unpause below is in this same call, and the next frame
    // start runs the warp with the game running.
    if (teleportRequested.exchange(false) && flying && camera)
    {
        // A held camera (a debug case, or automation) would keep photo mode on and the game paused, so
        // the warp would wait forever: teleport lets it go, from the pose it moved to this update.
        if constexpr (DevTools)
        {
            if (heldCamera) place(freeCamera, *heldCamera);
            held_camera::release();
            heldCamera.reset();
        }

        std::memcpy(warp.position, freeCamera.position, sizeof(warp.position));
        warp.yaw = static_cast<float>(std::remainder(freeCamera.yaw, 6.2831853f));
        warpPending = true;
        requested = false;
    }

    const bool wanted = requested || heldCamera;
    // Without a camera (a loading screen) the request lapses; a held camera waits for one.
    if (wanted && !flying && !camera) requested = false;
    else if (wanted && !flying)
    {
        freeCamera = fromGame(stateOf(camera));
        secondsSince(lastStep);
        mouseX = 0;
        mouseY = 0;
        pause(true);
        focus::setActive(true);
        flying = true;
    }
    else if (!wanted && flying)
    {
        pause(false);
        focus::setActive(false);
        flying = false;
    }

    if (!flying || !camera)
    {
        originalSelectorUpdate(selector);
        if constexpr (DevTools)
        {
            if (camera) held_camera::remember(poseOf(fromGame(stateOf(camera))));
        }

        return;
    }

    const float seconds = static_cast<float>(secondsSince(lastStep));
    const int turnX = mouseX.exchange(0), turnY = mouseY.exchange(0);
    const std::uint8_t keys = heldKeys;
    const auto game = stateOf(camera);
    if (heldCamera) place(freeCamera, *heldCamera);
    else
    {
        fly(freeCamera, keys, turnX, turnY, seconds);
        if (const float target = focus::distance(); target > 0)
            freeCamera.focusDistance += (target - freeCamera.focusDistance) * (1 - std::exp(-seconds / FocusTime));
    }

    // Only this update sees the free camera; the game's camera keeps its own.
    apply(camera, photoState(freeCamera, game, static_cast<bool>(heldCamera)));
    originalSelectorUpdate(selector);
    apply(camera, game);
    if constexpr (DevTools) held_camera::remember(poseOf(freeCamera));
}

// In photo mode the mouse turns the free camera and the game gets no presses. Releases still reach
// it, so nothing held when photo mode started stays held after it.
bool rawInput(void* devices, HRAWINPUT input, HWND window)
{
    if (!flying) return originalRawInput(devices, input, window);

    RAWINPUT data{};
    UINT size = sizeof(data);
    if (GetRawInputData(input, RID_INPUT, &data, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return originalRawInput(devices, input, window);

    if (data.header.dwType == RIM_TYPEMOUSE)
    {
        const auto& mouse = data.data.mouse;
        if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
        {
            mouseX += mouse.lLastX;
            mouseY += mouse.lLastY;
        }

        return mouse.usButtonFlags & ButtonReleases ? originalRawInput(devices, input, window) : true;
    }

    if (data.header.dwType == RIM_TYPEKEYBOARD) return data.data.keyboard.Flags & RI_KEY_BREAK ? originalRawInput(devices, input, window) : true;

    return originalRawInput(devices, input, window);
}
}

std::string hook() noexcept
{
    if (!isSupportedGameBuild()) return "Unknown game build; photo mode is unavailable.";

    auto* base = game_code::image();
    auto* manager = base + PauseManagerRva;
    if (std::memcmp(base + SelectorUpdateRva, SelectorUpdateStart, sizeof(SelectorUpdateStart))
        || std::memcmp(base + RawInputRva, RawInputStart, sizeof(RawInputStart))
        || std::memcmp(base + CreatePauseRva, CreatePauseStart, sizeof(CreatePauseStart))
        || *reinterpret_cast<std::uint8_t**>(manager) != base + PauseVtableRva)
        return "The camera, input or pause code is unexpected; photo mode is unavailable.";

    pauseManager = manager;
    createPause = reinterpret_cast<CreatePause>(base + CreatePauseRva);
    if (!std::memcmp(base + WeatherPauseTestsRva, WeatherPauseTests, sizeof(WeatherPauseTests))) weatherPauseTests = base + WeatherPauseTestsRva;
    auto status = game_code::hookAt(base + SelectorUpdateRva, &selectorUpdate, originalSelectorUpdate);
    if (status == MH_OK)
    {
        status = game_code::hookAt(base + RawInputRva, &rawInput, originalRawInput);
        if (status != MH_OK) MH_RemoveHook(base + SelectorUpdateRva);
    }

    if (status != MH_OK) return std::string("Hooking the camera failed (") + MH_StatusToString(status) + "); photo mode is unavailable.";

    installed = true;

    return "The camera selector and raw input are hooked: F2 toggles photo mode.";
}

void toggle() noexcept
{
    if (installed) requested = !requested;
}

void toggleUi() noexcept
{
    hideUi = !hideUi;
}

void toggleBlur() noexcept
{
    blur = !blur;
}

void configure(const Configuration& configuration) noexcept
{
    fNumber = configuration.settings.photoMode.fNumber;
}

void setControls(const Controls& controls) noexcept
{
    heldKeys = static_cast<std::uint8_t>((controls.forward ? Forward : 0) | (controls.back ? Back : 0) | (controls.left ? Left : 0)
        | (controls.right ? Right : 0) | (controls.up ? Up : 0) | (controls.down ? Down : 0) | (controls.fast ? Fast : 0)
        | (controls.slow ? Slow : 0));
}

bool uiHidden() noexcept
{
    return hideUi || flying;
}

void teleport() noexcept
{
    if constexpr (DevTools) teleportRequested = true;
}

void onFrameStart() noexcept
{
    if constexpr (DevTools)
    {
        // A dev build alone links the game's Lua; this body is not odr-used in a release build.
        if (!std::exchange(warpPending, false)) return;

        // The game's own warp, as Infinite Heaven's InfTppUtil.Warp sends it: WarpAndWaitBlock for the player
        // (object 0), SetPosition for the vehicle or walker gear the player is in. rotY takes radians.
        const auto script = std::format(R"(
if not (vars and GameObject and TppGameObject) then return "The game's scripts are not ready." end
local kind = math.floor(vars.missionCode / 10000)
if kind ~= 1 and kind ~= 3 then return "Only in a mission or free roam." end
local id = 0
if vars.playerVehicleGameObjectId and vars.playerVehicleGameObjectId ~= GameObject.NULL_ID then id = vars.playerVehicleGameObjectId end
local type = GameObject.GetTypeIndex(id)
local rotY = {:.5f}
local pos = {{{:.3f}, {:.3f}, {:.3f}}}
if type == TppGameObject.GAME_OBJECT_TYPE_PLAYER2 then
  GameObject.SendCommand(id, {{id = "WarpAndWaitBlock", pos = pos, rotY = rotY}})
elseif type == TppGameObject.GAME_OBJECT_TYPE_VEHICLE then
  GameObject.SendCommand(id, {{id = "SetPosition", position = Vector3(pos[1], pos[2], pos[3]), rotY = rotY}})
elseif type == TppGameObject.GAME_OBJECT_TYPE_WALKERGEAR2 or type == TppGameObject.GAME_OBJECT_TYPE_COMMON_WALKERGEAR2 then
  GameObject.SendCommand(id, {{id = "SetPosition", pos = pos, rotY = rotY}})
else
  return "The player is not on foot, in a vehicle or on walker gear."
end
)",
            warp.yaw, warp.position[0], warp.position[1], warp.position[2]);

        const auto outcome = game_lua::run(script, "=PhantomFX photo mode teleport");
        if (!outcome)
        {
            fxui::toast(fxui::Tone::Warn, "Teleport", "The game's scripts are not running yet.");
            return;
        }

        if (!outcome->ran)
        {
            log("Photo mode's teleport failed: " + outcome->text);
            fxui::toast(fxui::Tone::Bad, "Teleport failed", "See PhantomFX.log.");
            return;
        }

        if (!outcome->text.empty())
        {
            fxui::toast(fxui::Tone::Warn, "Teleport", outcome->text);
            return;
        }

        log(std::format("Photo mode: the player warped to {:.3f}, {:.3f}, {:.3f}.", warp.position[0], warp.position[1], warp.position[2]));
    }
}

void leave() noexcept
{
    if constexpr (DevTools) held_camera::release();

    requested = false;
    flying = false;
    teleportRequested = false;
    warpPending = false;
    pause(false);
    focus::setActive(false);
}

}
