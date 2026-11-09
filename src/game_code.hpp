#pragma once

#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

// The game's code: its image, checks that its code is the build's, hooks and patches.
namespace phantom_fx::game_code
{
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

// The bytes another mod's hook wrote over the start of a function: jmp rel32, or jmp [rip + 0] with its
// target after it; 0 when there is none.
inline std::size_t hookJumpLength(const std::uint8_t* at) noexcept
{
    if (at[0] == 0xE9) return 5;
    if (at[0] == 0xFF && at[1] == 0x25) return 14;
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

// On failure nothing is left hooked and `original` is null. The detour takes the original's type.
// presenter.cpp hooks DXGI with it too.
template<class Function> MH_STATUS hookAt(void* target, std::type_identity_t<Function> detour, Function& original) noexcept
{
    auto status = MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        MH_RemoveHook(target);
        original = nullptr;
    }

    return status;
}

// Writes over the game's code; false, with nothing written, when its page cannot be made writable.
inline bool patch(std::uint8_t* at, std::span<const unsigned char> code) noexcept
{
    DWORD protection{};
    if (!VirtualProtect(at, code.size(), PAGE_EXECUTE_READWRITE, &protection)) return false;

    std::memcpy(at, code.data(), code.size());
    VirtualProtect(at, code.size(), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), at, code.size());

    return true;
}
}
