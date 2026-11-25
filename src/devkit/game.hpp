#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// The game's own layer, as the add-on that uses the devkit has it: the build it knows, and checks on
// the code it expects to find. PhantomFX answers isSupportedBuild from src/device_check.cpp.
namespace devkit::game
{
// Whether mgsvtpp.exe is the build the add-on knows. Its addresses are the build's alone.
bool isSupportedBuild() noexcept;

inline std::uint8_t* image() noexcept
{
    return reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
}

// Code the game's build has at an address in its image.
struct Expected
{
    std::uintptr_t rva;
    std::span<const unsigned char> code;
};

inline bool matches(std::span<const Expected> expected) noexcept
{
    return std::ranges::all_of(expected, [](const Expected& one) { return std::memcmp(image() + one.rva, one.code.data(), one.code.size()) == 0; });
}

// The bytes another mod's hook wrote over the start of a function: jmp rel32, or jmp [rip + disp32] (14 with the
// target after it when disp32 is 0, 6 otherwise); 0 when there is none.
inline std::size_t hookJumpLength(const std::uint8_t* at) noexcept
{
    if (at[0] == 0xE9) return 5;
    if (at[0] == 0xFF && at[1] == 0x25) return (at[2] | at[3] | at[4] | at[5]) ? 6 : 14;
    return 0;
}

// As matches, but a function another mod hooked (Infinite Heaven's IHHook hooks the game's Lua) passes
// when what follows the hook's jump, and the int3 or nop filling the rest of the instruction it cut,
// is the build's code. Calls then go through the hook, which calls the game's function.
inline bool matchesPastHooks(std::span<const Expected> expected) noexcept
{
    return std::ranges::all_of(expected, [](const Expected& one) {
        const auto* at = image() + one.rva;
        auto from = hookJumpLength(at);
        while (from && from < one.code.size() && at[from] != one.code[from] && (at[from] == 0xCC || at[from] == 0x90)) ++from;

        return one.code.size() >= from + 8 && std::memcmp(at + from, one.code.data() + from, one.code.size() - from) == 0;
    });
}
}
