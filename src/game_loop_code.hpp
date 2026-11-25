#pragma once

#include <cstdint>

// The main loop's frame start, on the loop object (rcx): docs/DEVELOPMENT.md, "Frame generation". One
// copy for PhantomFX's own hook (game_loop.cpp) and the DevKit add-on's (devaddon/game_loop.cpp).
namespace phantom_fx::game_loop_code
{
inline constexpr std::uintptr_t FrameStartRva = 0x7F840;
// push rbx; push rsi; push rdi; sub rsp, 0x60; mov qword ptr [rsp + 0x20], -2
inline constexpr unsigned char FrameStartCode[]{0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF};
}
