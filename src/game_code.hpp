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

// Whether the calling thread's stack holds a return address in the game's image: the call comes from
// the game's code, not from another tool's thread.
inline bool calledFromGame() noexcept
{
    const auto* base = image();
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto begin = reinterpret_cast<std::uintptr_t>(base);
    const auto end = begin + nt->OptionalHeader.SizeOfImage;
    void* frames[62]{};
    const auto count = RtlCaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);

    return std::any_of(frames, frames + count, [&](void* frame)
    {
        const auto address = reinterpret_cast<std::uintptr_t>(frame);
        return address >= begin && address < end;
    });
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
