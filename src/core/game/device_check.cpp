#include "device_check.hpp"

#include "game_code.hpp"

#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace phantom_fx
{
namespace
{
// The check (0x1402B9BA0 in the 2026-07-07 build) enumerates the process's modules, finds
// d3d11.dll and returns false if any of 97 vtable entries lies outside it. Its caller then quits.
constexpr std::uint32_t TimeDateStamp = 0x6A4CB898, SizeOfImage = 0xA01D000;
constexpr std::uintptr_t CheckRva = 0x2B9BA0;
// push rbx; sub rsp, 0x460
constexpr unsigned char Original[]{0x40, 0x53, 0x48, 0x81, 0xEC, 0x60, 0x04, 0x00, 0x00};
// mov al, 1; ret
constexpr unsigned char Pass[]{0xB0, 0x01, 0xC3};

bool isKnownGameBuild(const std::uint8_t* base)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == TimeDateStamp
        && nt->OptionalHeader.SizeOfImage == SizeOfImage;
}
}

bool isSupportedGameBuild() noexcept
{
    return isKnownGameBuild(game_code::image());
}

std::string passDeviceCheck() noexcept
{
    auto* base = game_code::image();
    if (!isKnownGameBuild(base)) return "Unknown game build; the start-up device check is left alone.";

    auto* check = base + CheckRva;
    if (!std::memcmp(check, Pass, sizeof(Pass))) return "The start-up device check already passes.";
    if (std::memcmp(check, Original, sizeof(Original))) return "The start-up device check has unexpected code; left alone.";

    if (!game_code::patch(check, Pass)) return "Could not unprotect the start-up device check.";

    return "The start-up device check now passes.";
}
}
