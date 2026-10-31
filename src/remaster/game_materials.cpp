#include "remaster/game_materials.hpp"

#include "game_code.hpp"
#include "game_memory.hpp"

#include <MinHook.h>

namespace phantom_fx::remaster::game_materials
{
namespace
{
using game_memory::read;

constexpr std::uintptr_t MaterialPoolRva = 0x2B743B0, TechniquePoolRva = 0x2B743A8, MergeRva = 0x1C8960;

// A pool: its rows, how many, and a slot of bookkeeping for each row.
constexpr std::size_t PoolRows = 0x8, PoolCapacity = 0x10, PoolSlots = 0x20, SlotSize = 0xC, SlotUses = 0x4;
constexpr std::size_t MaterialSize = 0xD0, MaterialName = 0x8, MaterialTechnique = 0x20, MaterialRegisterCount = 0x24, MaterialRegisters = 0x30;
constexpr std::size_t TechniqueSize = 0x50, TechniqueName = 0x8, TechniqueParameters = 0x28, TechniquePacking = 0x30;
// A handle: the row << 17, and in the low bits what it is a handle to.
constexpr std::uint32_t HandleKindBits = 0x7F, HandleRowShift = 17, MaterialHandle = 7, TechniqueHandle = 4;

constexpr std::uint64_t NameBits = 0xFFFFFFFFFFFF;
constexpr std::uint32_t MostParameters = 64, MostRegisters = 8, ComponentsInRegister = 4;

// Its start, with the material pool's address: 0x2B743B0 is read by "mov rsi, [rip + 0x29ABA29]".
constexpr unsigned char MergeCode[]{0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x40, 0xA8,
    0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x58, 0x18, 0x8B, 0xE9, 0x48, 0x8B, 0x35, 0x29, 0xBA, 0x9A, 0x02};
const game_code::Expected Checked[]{{MergeRva, MergeCode}};

struct Pool
{
    std::uintptr_t rows{}, slots{};
    std::uint32_t capacity{};
};

std::optional<Pool> poolAt(std::uintptr_t rva) noexcept
{
    const auto pool = read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(game_code::image()) + rva);
    if (!pool || !*pool) return std::nullopt;

    const auto rows = read<std::uintptr_t>(*pool + PoolRows);
    const auto slots = read<std::uintptr_t>(*pool + PoolSlots);
    const auto capacity = read<std::uint32_t>(*pool + PoolCapacity);
    if (!rows || !slots || !capacity) return std::nullopt;

    return Pool{*rows, *slots, *capacity};
}

using Merge = std::uint32_t (*)(std::uint32_t handle);
Merge originalMerge{};
NewMaterial newMaterial{};

std::uint32_t mergeMaterial(std::uint32_t handle)
{
    if ((handle & HandleKindBits) == MaterialHandle)
        if (const auto material = materialAt(handle >> HandleRowShift)) newMaterial(*material);

    return originalMerge(handle);
}
}

std::optional<Material> materialAt(std::uint32_t row) noexcept
{
    const auto materials = poolAt(MaterialPoolRva);
    const auto techniques = poolAt(TechniquePoolRva);
    if (!materials || !techniques || row >= materials->capacity) return std::nullopt;

    const auto at = materials->rows + std::uintptr_t{row} * MaterialSize;
    const auto name = read<std::uint64_t>(at + MaterialName);
    const auto technique = read<std::uint32_t>(at + MaterialTechnique);
    const auto registers = read<std::uint8_t>(at + MaterialRegisterCount);
    const auto uses = read<std::uint32_t>(materials->slots + std::uintptr_t{row} * SlotSize + SlotUses);
    if (!name || !technique || !registers || !uses || *registers > MostRegisters || (*technique & HandleKindBits) != TechniqueHandle) return std::nullopt;

    const auto techniqueRow = *technique >> HandleRowShift;
    if (techniqueRow >= techniques->capacity) return std::nullopt;

    const auto techniqueAt = techniques->rows + std::uintptr_t{techniqueRow} * TechniqueSize;
    const auto techniqueName = read<std::uint64_t>(techniqueAt + TechniqueName);
    const auto parameters = read<std::uintptr_t>(techniqueAt + TechniqueParameters);
    const auto packing = read<std::uintptr_t>(techniqueAt + TechniquePacking);
    if (!techniqueName || !parameters || !packing) return std::nullopt;

    const auto parameterCount = read<std::uint32_t>(*parameters);
    const auto parameterNames = read<std::uintptr_t>(*parameters + 8);
    const auto packingBytes = read<std::uintptr_t>(*packing + 8);
    if (!parameterCount || !parameterNames || !packingBytes || !*parameterNames || !*packingBytes || !*parameterCount || *parameterCount > MostParameters) return std::nullopt;

    const std::size_t components = std::size_t{*registers} * ComponentsInRegister;

    return Material{.index = row,
        .name = *name & NameBits,
        .technique = *techniqueName & NameBits,
        .uses = *uses,
        .packed = {.parameters = {reinterpret_cast<const std::uint32_t*>(*parameterNames), *parameterCount},
            .packing = {reinterpret_cast<const std::uint8_t*>(*packingBytes), components},
            .registers = {reinterpret_cast<float*>(at + MaterialRegisters), components}}};
}

void forEachMaterial(const std::function<void(const Material&)>& visit)
{
    const auto materials = poolAt(MaterialPoolRva);
    if (!materials) return;

    for (std::uint32_t row = 0; row < materials->capacity; ++row)
        if (const auto material = materialAt(row); material && static_cast<int>(material->uses) > 0) visit(*material);
}

std::string hookNewMaterials(NewMaterial onNew) noexcept
{
    if (!game_code::matches(Checked)) return "the game's code differs";

    newMaterial = onNew;
    const auto status = game_code::hookAt(game_code::image() + MergeRva, mergeMaterial, originalMerge);

    return status == MH_OK ? "" : MH_StatusToString(status);
}
}
