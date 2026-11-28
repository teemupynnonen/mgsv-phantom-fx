#include "weather.hpp"

#include "device_check.hpp"
#include "game_code.hpp"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <mutex>
#include <utility>

namespace phantom_fx::weather
{
namespace
{
// The WeatherManager, made on first use and kept for the session: a pointer at ManagerRva. The
// object at + 0x58 holds the weather requests, the one at + 0x80 the clock.
constexpr std::uintptr_t ManagerRva = 0x2BD3600;
constexpr std::size_t Requests = 0x58, Clock = 0x80;
// The clock's seconds since midnight, a float, and the byte that stops it (PauseClock).
constexpr std::size_t ClockSeconds = 0xED0, ClockStopped = 0xEC8;
constexpr float LastSecond = 86399;

// The requests: from + 0x2C one 16-byte slot per priority, REQUEST_PRIORITY_NORMAL (0) to DEMO (4),
// and the strongest with a weather type wins. A slot holds its user (a name hash), transition time,
// weather type (a byte, 0xFF for none) and, for fog, its density plus 10000 x its fog type. From
// + 0xC1C one byte per weather type, set when the location loads its settings for that weather;
// the game ignores requests for the others. + 0xE0 is what applying a request changes.
constexpr std::size_t Slots = 0x2C, SlotSize = 16, SlotUser = 0, SlotTransition = 4, SlotType = 8, SlotFog = 12;
constexpr std::size_t TypeAvailable = 0xC1C, Effects = 0xE0;
constexpr int Priorities = 5, ForcePriority = 3;
constexpr std::uint8_t NoWeather = 0xFF;
// The menu's requests carry this user; the game's are hashes of names such as "Script".
constexpr std::uint32_t MenuUser = 0x534C4447;
constexpr int NormalFog = 1; // FOG_TYPE_NORMAL
constexpr float FogTypeStep = 10000;
// A transition of 0 takes the game's default one; under 1e-5 the weather switches at once.
constexpr float Instant = 1e-6f;

// What the script functions call. SetCurrentClock (0x5365C0) passes whole seconds; this also tells
// an object reached from ListenersRva (see ready). RequestWeather (0x5361D0) fills the priority's
// slot and applies the strongest request; for fog it then sets the density and the fog type, which
// only change a slot whose user and type match. CancelRequestWeather (0x534680) empties the slot
// when the user matches, and applies.
constexpr std::uintptr_t SetClockRva = 0x536550, RequestRva = 0x551430, FogDensityRva = 0x551B80, FogTypeRva = 0x551BB0,
                         CancelRva = 0x54F8F0;
using SetClock = void (*)(std::uint8_t* manager, std::uint32_t seconds);
using Request = void (*)(std::uint8_t* requests, int priority, std::uint32_t user, int type, float transition);
using FogDensity = void (*)(std::uint8_t* requests, int priority, std::uint32_t user, float density);
using FogType = void (*)(std::uint8_t* requests, int priority, std::uint32_t user, int fogType);
using Cancel = void (*)(std::uint8_t* requests, int priority, std::uint32_t user);
// The global of the singleton made by 0xBFEFA0; SetCurrentClock calls the first virtual of
// [[[[global + 8] + 0x98] + 0x78] + 8], and requests read [[global + 8] + 0x98] + 0x10.
constexpr std::uintptr_t ListenersRva = 0x2C3A8A0;

// The code read for each address above, checked before the menu touches anything.
// RestoreFromSVars: sub rsp, 0x28; mov rcx, [ManagerRva]; mov rcx, [rcx + 0x58]
constexpr unsigned char RestoreCode[]{0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x0D, 0xA5, 0xD1, 0x69, 0x02, 0x48, 0x8B, 0x49, 0x58};
// The clock's getter: mov r8, [rcx + 0x80]; mov eax, [r8 + 0xED0]; mov [rdx], eax; cmp byte ptr [r8 + 0xEC8], 0
constexpr unsigned char ClockCode[]{0x4C, 0x8B, 0x81, 0x80, 0x00, 0x00, 0x00, 0x41, 0x8B, 0x80, 0xD0, 0x0E, 0x00, 0x00, 0x89, 0x02,
    0x41, 0x80, 0xB8, 0xC8, 0x0E, 0x00, 0x00, 0x00};
// Whether a type is available: cmp edx, 5; jl; xor al, al; ret; movsxd rax, edx; movzx eax, byte ptr [rax + rcx + 0xC1C]; ret
constexpr unsigned char TypeCode[]{0x83, 0xFA, 0x05, 0x7C, 0x03, 0x32, 0xC0, 0xC3, 0x48, 0x63, 0xC2, 0x0F, 0xB6, 0x84, 0x08, 0x1C, 0x0C, 0x00, 0x00, 0xC3};
// mov [rsp + 8], rbx; push rdi; sub rsp, 0x20; mov r8, [rcx + 0x80]; xorps xmm0, xmm0; mov ebx, edx;
// mov rax, [r8 + 0xEF8]; mov rdi, rcx; cvtsi2ss xmm0, rbx; movss [r8 + 0xED0], xmm0
constexpr unsigned char SetClockCode[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x81, 0x80, 0x00, 0x00,
    0x00, 0x0F, 0x57, 0xC0, 0x8B, 0xDA, 0x49, 0x8B, 0x80, 0xF8, 0x0E, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0xF3, 0x48, 0x0F, 0x2A, 0xC3, 0xF3,
    0x41, 0x0F, 0x11, 0x80, 0xD0, 0x0E, 0x00, 0x00};
// movss xmm0, [rsp + 0x28]; movsxd rax, edx; add rax, rax; movss [rcx + rax * 8 + 0x30], xmm0;
// mov [rcx + rax * 8 + 0x2C], r8d; mov [rcx + rax * 8 + 0x34], r9b
constexpr unsigned char RequestCode[]{0xF3, 0x0F, 0x10, 0x44, 0x24, 0x28, 0x48, 0x63, 0xC2, 0x48, 0x03, 0xC0, 0xF3, 0x0F, 0x11, 0x44,
    0xC1, 0x30, 0x44, 0x89, 0x44, 0xC1, 0x2C, 0x44, 0x88, 0x4C, 0xC1, 0x34};
// movsxd rax, edx; add rax, rax; cmp [rcx + rax * 8 + 0x2C], r8d; jne; cmp byte ptr [rcx + rax * 8 + 0x34], 4; jne
constexpr unsigned char FogDensityCode[]{0x48, 0x63, 0xC2, 0x48, 0x03, 0xC0, 0x44, 0x39, 0x44, 0xC1, 0x2C, 0x75, 0x12, 0x80, 0x7C, 0xC1,
    0x34, 0x04, 0x75};
// The same with r10
constexpr unsigned char FogTypeCode[]{0x4C, 0x63, 0xD2, 0x4D, 0x03, 0xD2, 0x46, 0x39, 0x44, 0xD1, 0x2C, 0x75, 0x53, 0x42, 0x80, 0x7C,
    0xD1, 0x34, 0x04, 0x75, 0x4B};
// movsxd rax, edx; add rax, rax; cmp [rcx + rax * 8 + 0x2C], r8d; jne; xor r8d, r8d; mov dword ptr [rcx + rax * 8 + 0x2C], 0xBF169F98
constexpr unsigned char CancelCode[]{0x48, 0x63, 0xC2, 0x48, 0x03, 0xC0, 0x44, 0x39, 0x44, 0xC1, 0x2C, 0x75, 0x1F, 0x45, 0x33, 0xC0,
    0xC7, 0x44, 0xC1, 0x2C, 0x98, 0x9F, 0x16, 0xBF};
// sub rsp, 0x38; mov qword ptr [rsp + 0x20], -2; mov rax, [ListenersRva]; test rax, rax
constexpr unsigned char ListenersCode[]{0x48, 0x83, 0xEC, 0x38, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0x05,
    0xEC, 0xB8, 0x03, 0x02, 0x48, 0x85, 0xC0};

const game_code::Expected Checked[]{{0x536450, RestoreCode}, {0x5352A0, ClockCode}, {0x550C90, TypeCode}, {SetClockRva, SetClockCode},
    {RequestRva, RequestCode}, {FogDensityRva, FogDensityCode}, {FogTypeRva, FogTypeCode}, {CancelRva, CancelCode},
    {0xBFEFA0, ListenersCode}};

// What the menu asked for since the last frame start.
struct Pending
{
    std::optional<float> clock;
    std::optional<bool> clockStopped;
    bool weatherChanged{};
    std::optional<Type> weather; // Empty releases the menu's request
    float fogDensity{};
};

std::mutex mutex;
Pending pending;
State published;

template<class Value> Value& at(std::uint8_t* object, std::size_t offset)
{
    return *reinterpret_cast<Value*>(object + offset);
}

// Follows pointers from `object` through each offset; null if one is.
std::uint8_t* follow(std::uint8_t* object, std::initializer_list<std::size_t> offsets)
{
    for (const auto offset : offsets)
    {
        if (!object) return nullptr;

        object = at<std::uint8_t*>(object, offset);
    }

    return object;
}

// Outside a mission or free roam, the objects the clock and requests reach are missing.
bool ready(std::uint8_t* manager)
{
    auto* listeners = at<std::uint8_t*>(game_code::image(), ListenersRva);

    return follow(manager, {Requests}) && follow(manager, {Clock}) && follow(manager, {Requests, Effects})
        && follow(listeners, {0x8, 0x98, 0x78, 0x8}) && follow(listeners, {0x8, 0x98, 0x10});
}

std::uint8_t* slot(std::uint8_t* requests, int priority)
{
    return requests + Slots + priority * SlotSize;
}

std::optional<Type> typeOf(std::uint8_t* slot)
{
    const auto type = at<std::uint8_t>(slot, SlotType);
    if (type == NoWeather || type >= static_cast<std::uint8_t>(Type::Count)) return std::nullopt;

    return static_cast<Type>(type);
}

// The strongest request other than the forced one.
std::uint8_t* gameRequest(std::uint8_t* requests)
{
    for (int priority = Priorities - 1; priority >= 0; --priority)
        if (priority != ForcePriority && typeOf(slot(requests, priority))) return slot(requests, priority);

    return nullptr;
}

void apply(std::uint8_t* manager, const Pending& wanted)
{
    auto* const engine = game_code::image();
    auto* const requests = at<std::uint8_t*>(manager, Requests);
    if (wanted.clock)
        reinterpret_cast<SetClock>(engine + SetClockRva)(manager, static_cast<std::uint32_t>(std::clamp(*wanted.clock, 0.0f, LastSecond)));
    if (wanted.clockStopped) at<std::uint8_t>(at<std::uint8_t*>(manager, Clock), ClockStopped) = *wanted.clockStopped;
    if (!wanted.weatherChanged) return;

    if (!wanted.weather)
    {
        // The game's request takes over with the transition it was made with (20 for the weather
        // system's), which in testing left the forced weather in place; switch back at once instead.
        if (auto* const game = gameRequest(requests)) at<float>(game, SlotTransition) = Instant;
        reinterpret_cast<Cancel>(engine + CancelRva)(requests, ForcePriority, MenuUser);
        return;
    }

    reinterpret_cast<Request>(engine + RequestRva)(requests, ForcePriority, MenuUser, static_cast<int>(*wanted.weather), Instant);
    if (*wanted.weather != Type::Foggy) return;

    reinterpret_cast<FogDensity>(engine + FogDensityRva)(requests, ForcePriority, MenuUser, wanted.fogDensity);
    reinterpret_cast<FogType>(engine + FogTypeRva)(requests, ForcePriority, MenuUser, NormalFog);
}

State read(std::uint8_t* manager)
{
    auto* const requests = at<std::uint8_t*>(manager, Requests);
    auto* const clock = at<std::uint8_t*>(manager, Clock);
    State now{.available = true, .clock = at<float>(clock, ClockSeconds), .clockStopped = at<std::uint8_t>(clock, ClockStopped) != 0};
    for (std::size_t type = 0; type < now.typeAvailable.size(); ++type) now.typeAvailable[type] = at<std::uint8_t>(requests, TypeAvailable + type) != 0;

    for (int priority = Priorities - 1; priority >= 0 && !now.requested; --priority) now.requested = typeOf(slot(requests, priority));

    auto* const forced = slot(requests, ForcePriority);
    if (at<std::uint32_t>(forced, SlotUser) == MenuUser)
    {
        now.forced = typeOf(forced);
        now.fogDensity = std::fmod(at<float>(forced, SlotFog), FogTypeStep);
    }

    return now;
}
}

bool supported() noexcept
{
    static const bool expected = isSupportedGameBuild() && game_code::matches(Checked);

    return expected;
}

// The game's pause manager (docs/DEVELOPMENT.md, "Photo mode"), a global with its vtable first: each frame start ORs
// its active requests' pause bits into + 8, and game time steps by zero while any is set.
bool isGamePaused() noexcept
{
    constexpr std::uintptr_t PauseManagerRva = 0x2B65D40, PauseVtableRva = 0x20C56C0;
    constexpr std::size_t PauseBits = 8;
    static const std::uint8_t* const manager = [] {
        auto* base = game_code::image();
        return isSupportedGameBuild() && *reinterpret_cast<std::uint8_t* const*>(base + PauseManagerRva) == base + PauseVtableRva
            ? base + PauseManagerRva
            : nullptr;
    }();
    return manager && *reinterpret_cast<const volatile std::uint32_t*>(manager + PauseBits) != 0;
}

State state() noexcept
{
    std::scoped_lock lock(mutex);

    return published;
}

void setClock(float seconds) noexcept
{
    std::scoped_lock lock(mutex);
    pending.clock = seconds;
}

void stopClock(bool stopped) noexcept
{
    std::scoped_lock lock(mutex);
    pending.clockStopped = stopped;
}

void force(Type type, float fogDensity) noexcept
{
    std::scoped_lock lock(mutex);
    pending.weatherChanged = true;
    pending.weather = type;
    pending.fogDensity = fogDensity;
}

void release() noexcept
{
    std::scoped_lock lock(mutex);
    pending.weatherChanged = true;
    pending.weather.reset();
}

std::string_view label(Type type) noexcept
{
    constexpr std::string_view labels[]{"Sunny", "Cloudy", "Rainy", "Sandstorm", "Foggy"};

    return type < Type::Count ? labels[static_cast<std::size_t>(type)] : "Unknown";
}

void onFrameStart() noexcept
{
    if (!supported()) return;

    Pending wanted;
    {
        std::scoped_lock lock(mutex);
        wanted = std::exchange(pending, {});
    }

    auto* const manager = at<std::uint8_t*>(game_code::image(), ManagerRva);
    State now;
    if (manager && ready(manager))
    {
        apply(manager, wanted);
        now = read(manager);
    }

    std::scoped_lock lock(mutex);
    published = now;
}
}
