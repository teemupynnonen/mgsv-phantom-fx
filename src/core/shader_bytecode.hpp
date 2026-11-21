#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phantom_fx
{
// The 16-byte checksum in a DXBC container's header (bytes 4 to 20). It identifies a shader
// across sessions, since the game loads the same precompiled bytecode each time.
using DxbcHash = std::array<std::uint8_t, 16>;

// The checksum of `code`, or nullopt when it is not a DXBC container.
std::optional<DxbcHash> dxbcHashOf(const void* code, std::size_t size);
// The checksum as 32 lowercase hex digits, the form docs/DEVELOPMENT.md lists.
std::string toHex(const DxbcHash& hash);

// The checksum a container's header must hold for Direct3D to take it: MD5 over everything after
// the header, with the format's own last block. For changed bytecode.
DxbcHash dxbcChecksum(const void* code, std::size_t size);
// Writes the checksum after the container's bytes changed.
void resign(std::vector<std::uint8_t>& code);

// One instruction of a shader program, in the shader token format (d3d11TokenizedProgramFormat.hpp
// in the Windows Driver Kit).
struct Instruction
{
    std::size_t offset{};   // Of its opcode token, in bytes from the container's start
    std::uint32_t opcode{}; // D3D10_SB_OPCODE_TYPE
    std::size_t length{};   // In tokens
};

// The program's instructions; nullopt when the code is not shader bytecode or is malformed.
std::optional<std::vector<Instruction>> instructionsOf(const std::uint8_t* code, std::size_t size);
// Where the instruction's four-component 32-bit literals are: the offset of each one's first value.
std::vector<std::size_t> immediatesOf(const std::uint8_t* code, const Instruction& instruction);
// A copy of a pixel shader with every write to its first render target (o0) saturated, as an UNORM
// target clamps what is written to it. Nullopt when the code is not shader bytecode or a write
// cannot be saturated.
std::optional<std::vector<std::uint8_t>> saturateOutput(const void* code, std::size_t size);
// A copy of a game pixel shader whose 8x8 colour dither, its `inMesh` texture, reads the texture's
// average in every pixel: a half in red, a quarter in alpha. The dither then adds what it adds on
// average. Nullopt when the shader has no such texture or never samples it.
std::optional<std::vector<std::uint8_t>> withoutDither(const void* code, std::size_t size);
// The game's shaders keep their source path in their debug data. True for a path ...\Draw2D*<suffix>,
// such as "_vs.hlsl": the 2D pass's. Other shaders mention Draw2D only through shared headers.
bool draw2DSource(std::string_view code, std::string_view suffix);
// The file name before `suffix` (such as "_vs.hlsl") of the first source path in the debug data
// that ends in it: "fox3ddf_blin" for ...\Release\fox3ddf_blin_vs.hlsl. Empty when there is none.
std::string_view sourceName(std::string_view code, std::string_view suffix);
}
