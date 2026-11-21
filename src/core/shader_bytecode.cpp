#include "shader_bytecode.hpp"

#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>

namespace phantom_fx
{
namespace
{
std::uint32_t wordAt(const std::uint8_t* bytes, std::size_t offset)
{
    std::uint32_t word{};
    std::memcpy(&word, bytes + offset, sizeof(word));

    return word;
}

// One MD5 block (RFC 1321).
void md5Block(std::array<std::uint32_t, 4>& state, const std::uint8_t* block)
{
    static constexpr std::uint32_t Shifts[64]{7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9,
        14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6,
        10, 15, 21};
    static constexpr std::uint32_t Constants[64]{0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562,
        0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87,
        0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44,
        0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8,
        0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f,
        0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

    auto [a, b, c, d] = state;
    for (std::uint32_t i = 0; i < 64; ++i)
    {
        std::uint32_t mixed{}, word{};
        if (i < 16) mixed = (b & c) | (~b & d), word = i;
        else if (i < 32) mixed = (d & b) | (~d & c), word = (5 * i + 1) % 16;
        else if (i < 48) mixed = b ^ c ^ d, word = (3 * i + 5) % 16;
        else mixed = c ^ (b | ~d), word = (7 * i) % 16;

        mixed += a + Constants[i] + wordAt(block, word * 4);
        a = d;
        d = c;
        c = b;
        b += std::rotl(mixed, static_cast<int>(Shifts[i]));
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

// The shader token format (d3d11TokenizedProgramFormat.hpp in the Windows Driver Kit).
constexpr std::uint32_t CustomData = 0x35, Mov = 0x36, Nop = 0x3A, Sample = 0x45, DeclareOutput = 0x65, Saturate = 1u << 13;
constexpr std::uint32_t OutputOperand = 2, Immediate32Operand = 4, ResourceOperand = 7, SwizzleMode = 1;
// A four-component immediate operand's token: its four values follow.
constexpr std::uint32_t Immediate4 = Immediate32Operand << 12 | 2;
// Instructions that write floats and take a saturate: add, div, dp2, dp3, dp4, mad, min, max, mov, movc, mul.
constexpr std::uint32_t Saturable[]{0x00, 0x0E, 0x0F, 0x10, 0x11, 0x32, 0x33, 0x34, 0x36, 0x37, 0x38};

// Where the operand at `operand` ends: its token, an extended token when bit 31 is set, then
// immediates or indices. Relative indices hold an operand of their own.
std::size_t operandEnd(const std::uint8_t* code, std::size_t operand)
{
    const auto token = wordAt(code, operand);
    operand += (token >> 31) ? 8 : 4;
    if (((token >> 12) & 0xFF) == Immediate32Operand) return operand + ((token & 3) == 2 ? 16 : 4);

    for (std::uint32_t dimension = 0; dimension < ((token >> 20) & 3); ++dimension)
    {
        const auto representation = (token >> (22 + 3 * dimension)) & 7;
        if (representation == 0 || representation == 3) operand += 4;
        if (representation == 1 || representation == 4) operand += 8;
        if (representation >= 2) operand = operandEnd(code, operand);
    }

    return operand;
}

// Where each of the instruction's operands starts, after its opcode and extended tokens.
std::vector<std::size_t> operandsOf(const std::uint8_t* code, const Instruction& instruction)
{
    const std::size_t end = instruction.offset + 4 * instruction.length;
    std::size_t at = instruction.offset + 4;
    for (bool extended = wordAt(code, instruction.offset) >> 31; extended && at < end; at += 4) extended = wordAt(code, at) >> 31;

    std::vector<std::size_t> operands;
    for (; at < end; at = operandEnd(code, at)) operands.push_back(at);

    return operands;
}

// True for the operand t<slot>.
bool isTexture(const std::uint8_t* code, std::size_t operand, std::uint32_t slot)
{
    const auto token = wordAt(code, operand);
    const bool hasImmediateIndex = ((token >> 20) & 3) == 1 && ((token >> 22) & 7) == 0;

    return ((token >> 12) & 0xFF) == ResourceOperand && hasImmediateIndex && wordAt(code, operand + ((token >> 31) ? 8 : 4)) == slot;
}
}

std::optional<DxbcHash> dxbcHashOf(const void* code, std::size_t size)
{
    if (!code || size < 32 || std::memcmp(code, "DXBC", 4)) return std::nullopt;

    DxbcHash hash{};
    std::memcpy(hash.data(), static_cast<const std::uint8_t*>(code) + 4, hash.size());

    return hash;
}

std::string toHex(const DxbcHash& hash)
{
    constexpr char Digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : hash)
    {
        text += Digits[byte >> 4];
        text += Digits[byte & 0xF];
    }

    return text;
}

DxbcHash dxbcChecksum(const void* code, std::size_t size)
{
    const auto* data = static_cast<const std::uint8_t*>(code) + 20;
    const std::size_t length = size - 20;
    const auto bits = static_cast<std::uint32_t>(length * 8);
    std::array<std::uint32_t, 4> state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const std::size_t whole = length / 64 * 64;
    for (std::size_t offset = 0; offset < whole; offset += 64) md5Block(state, data + offset);

    // Unlike MD5's padding, the length comes first in the last block, and its last word holds
    // bits / 4 with the low bit set.
    const std::size_t rest = length - whole;
    const std::uint32_t closing = (bits >> 2) | 1;
    std::uint8_t block[64]{};
    if (rest < 56)
    {
        std::memcpy(block, &bits, 4);
        std::memcpy(block + 4, data + whole, rest);
        block[4 + rest] = 0x80;
        std::memcpy(block + 60, &closing, 4);
        md5Block(state, block);
    }
    else
    {
        std::memcpy(block, data + whole, rest);
        block[rest] = 0x80;
        md5Block(state, block);
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, &bits, 4);
        std::memcpy(block + 60, &closing, 4);
        md5Block(state, block);
    }

    DxbcHash hash{};
    std::memcpy(hash.data(), state.data(), hash.size());

    return hash;
}

std::optional<std::vector<Instruction>> instructionsOf(const std::uint8_t* code, std::size_t size)
{
    if (!dxbcHashOf(code, size)) return std::nullopt;

    const auto chunks = wordAt(code, 28);
    if (32 + 4ull * chunks > size) return std::nullopt;

    for (std::uint32_t chunk = 0; chunk < chunks; ++chunk)
    {
        const std::size_t offset = wordAt(code, 32 + 4 * chunk);
        if (offset + 16 > size || (std::memcmp(code + offset, "SHEX", 4) && std::memcmp(code + offset, "SHDR", 4))) continue;

        // The program: a version token, its length in words, then the instructions.
        const std::size_t start = offset + 8, end = start + 4ull * wordAt(code, start + 4);
        if (end > size) return std::nullopt;

        std::vector<Instruction> program;
        for (std::size_t at = start + 8; at < end;)
        {
            const auto token = wordAt(code, at);
            const auto opcode = token & 0x7FF;
            const std::size_t length = opcode == CustomData ? wordAt(code, at + 4) : (token >> 24) & 0x7F;
            if (!length || at + 4 * length > end) return std::nullopt;

            program.push_back({at, opcode, length});
            at += 4 * length;
        }

        return program;
    }

    return std::nullopt;
}

std::vector<std::size_t> immediatesOf(const std::uint8_t* code, const Instruction& instruction)
{
    std::vector<std::size_t> found;
    for (const auto operand : operandsOf(code, instruction))
    {
        const auto token = wordAt(code, operand);
        if (((token >> 12) & 0xFF) == Immediate32Operand && (token & 3) == 2) found.push_back(operand + ((token >> 31) ? 8 : 4));
    }

    return found;
}

void resign(std::vector<std::uint8_t>& code)
{
    const auto checksum = dxbcChecksum(code.data(), code.size());
    std::memcpy(code.data() + 4, checksum.data(), checksum.size());
}

std::optional<std::vector<std::uint8_t>> saturateOutput(const void* code, std::size_t size)
{
    std::vector<std::uint8_t> patched(static_cast<const std::uint8_t*>(code), static_cast<const std::uint8_t*>(code) + size);
    auto* bytes = patched.data();
    const auto program = instructionsOf(bytes, size);
    if (!program) return std::nullopt;

    for (const auto& instruction : *program)
    {
        // The destination follows the opcode and its extended tokens.
        const std::size_t end = instruction.offset + 4 * instruction.length;
        const auto token = wordAt(bytes, instruction.offset);
        std::size_t operand = instruction.offset + 4;
        for (bool extended = token >> 31; extended && operand < end; operand += 4) extended = wordAt(bytes, operand) >> 31;
        if (instruction.opcode == CustomData || instruction.opcode == DeclareOutput || operand + 8 > end) continue;

        const auto destination = wordAt(bytes, operand);
        const bool immediateIndex = ((destination >> 20) & 3) == 1 && ((destination >> 22) & 7) == 0;
        const std::size_t index = operand + ((destination >> 31) ? 8 : 4);
        if (((destination >> 12) & 0xFF) != OutputOperand || !immediateIndex || index + 4 > end || wordAt(bytes, index) != 0) continue;
        if (std::find(std::begin(Saturable), std::end(Saturable), instruction.opcode) == std::end(Saturable)) return std::nullopt;

        const auto saturated = token | Saturate;
        std::memcpy(bytes + instruction.offset, &saturated, 4);
    }

    resign(patched);

    return patched;
}

std::optional<std::vector<std::uint8_t>> withoutDither(const void* code, std::size_t size)
{
    // Most shaders never mention the name; only those that do are reflected.
    constexpr std::string_view DitherName = "inMesh";
    if (!code || std::string_view(static_cast<const char*>(code), size).find(DitherName) == std::string_view::npos) return std::nullopt;

    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    D3D11_SHADER_INPUT_BIND_DESC dither{};
    if (FAILED(D3DReflect(code, size, IID_PPV_ARGS(reflection.GetAddressOf())))
        || FAILED(reflection->GetResourceBindingDescByName(DitherName.data(), &dither)) || dither.Type != D3D_SIT_TEXTURE)
        return std::nullopt;

    std::vector<std::uint8_t> patched(static_cast<const std::uint8_t*>(code), static_cast<const std::uint8_t*>(code) + size);
    auto* bytes = patched.data();
    const auto program = instructionsOf(bytes, size);
    if (!program) return std::nullopt;

    // The game's dither textures average a half in red and a quarter in alpha (64 of 255; one 71);
    // green and blue hold 1 of 255.
    constexpr float Averages[]{0.5f, 1.0f / 255, 1.0f / 255, 0.25f};
    bool isPatched{};
    for (const auto& instruction : *program)
    {
        if (instruction.opcode != Sample) continue;

        // A sample's operands: destination, address, texture, sampler.
        const auto operands = operandsOf(bytes, instruction);
        if (operands.size() != 4 || !isTexture(bytes, operands[2], dither.BindPoint)) continue;

        const auto texture = wordAt(bytes, operands[2]);
        if (((texture >> 2) & 3) != SwizzleMode) return std::nullopt;

        // In its place: mov <destination>, the averages in the sample's swizzle; then nops to its length.
        std::vector<std::uint32_t> words{Mov};
        for (auto at = operands[0]; at < operands[1]; at += 4) words.push_back(wordAt(bytes, at));

        words.push_back(Immediate4);
        for (std::uint32_t component = 0; component < 4; ++component)
            words.push_back(std::bit_cast<std::uint32_t>(Averages[(texture >> (4 + 2 * component)) & 3]));

        if (words.size() > instruction.length) return std::nullopt;

        words[0] |= static_cast<std::uint32_t>(words.size()) << 24;
        words.resize(instruction.length, Nop | 1u << 24);
        std::memcpy(bytes + instruction.offset, words.data(), 4 * words.size());
        isPatched = true;
    }

    if (!isPatched) return std::nullopt;

    resign(patched);

    return patched;
}

bool draw2DSource(std::string_view code, std::string_view suffix)
{
    constexpr std::string_view Prefix = "\\Draw2D";
    for (auto at = code.find(Prefix); at != code.npos; at = code.find(Prefix, at + 1))
    {
        auto end = at + Prefix.size();
        while (end < code.size() && (std::isalnum(static_cast<unsigned char>(code[end])) || code[end] == '_')) ++end;
        // The name ends in _vs or _ps, followed by .hlsl.
        if (end >= 3 && code.substr(end - 3, suffix.size()) == suffix) return true;
    }

    return false;
}

std::string_view sourceName(std::string_view code, std::string_view suffix)
{
    const auto at = code.find(suffix);
    if (at == code.npos) return {};

    auto start = at;
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(code[start - 1])) || code[start - 1] == '_')) --start;

    return start > 0 && code[start - 1] == '\\' ? code.substr(start, at - start) : std::string_view{};
}
}
