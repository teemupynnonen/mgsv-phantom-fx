// The game's object brush; engine_brush.hpp describes it.
#include "passes/ray_tracing/engine_brush.hpp"

#include "game_code.hpp"
#include "game_memory.hpp"
#include "passes/ray_tracing/half_float.hpp"

#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace phantom_fx::engine_brush
{
namespace
{
using game_memory::isA;
using game_memory::read;
using game_memory::readRaw;

// The brush (ObjectBrush) and its blocks (ObjectBrushBlock): constructors, then the destructor, each,
// and their first bytes on the supported build.
constexpr std::uintptr_t BrushVtable = 0x2165200, BlockVtable = 0x2165350;
constexpr std::uintptr_t Functions[]{0x483460, 0x483550, 0x483A80, 0x483650, 0x483730, 0x483860, 0x483B20};
constexpr unsigned char Starts[std::size(Functions)][12]{
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30},
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC},
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7},
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30},
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40},
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30},
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7},
};
constexpr std::size_t BrushDestructor = 2, BlockDestructor = 6;

// A brush: its name (the string object its blocks name it by) at +0x48, its plugins' count at +0x130
// and their links at +0x138 (a null link where a plugin index is unused); a link's plugin at +0x18.
constexpr std::size_t BrushName = 0x48, PluginCount = 0x130, PluginLinks = 0x138, LinkPlugin = 0x18;
constexpr std::uint32_t MaxPlugins = 256;
// The plugins that draw a model (ObjectBrushPluginClone, TppObjectBrushPluginBush,
// TppObjectBrushPluginFlutteringGrass): its resource at +0x90, its scale range at +0xA8 and +0xAC.
constexpr std::uintptr_t PluginVtables[]{0x2166630, 0x21A63F0, 0x21AD270};
constexpr std::size_t PluginModel = 0x90, PluginScale = 0xA8;
// A block: its brush's name at +0xA0, its placements at +0x98, its id at +0xB8 (the low 16 bits: row x
// 64 + column of the brush's grid). The placements start with their groups' range (begin, end), the
// groups after 24 bytes in, 16 bytes each: the records' address (64 bits), their count (16), the plugin
// index (8), 1 (8) and a type (32); each record 24 bytes: Y (float), X and Z (16 bits each, around the
// block's centre, 255 a metre), the rotation (x, y, z, w, halves), the block's id (16 bits), the plugin
// index (8), the scale (8: 0 to 255 across the plugin's range) and an id (32). A group list ends with
// others' data, so only records naming their block and group's plugin count (all do in game: 46,359
// records in 234 groups of 25 blocks, none repeated).
constexpr std::size_t BlockName = 0xA0, BlockPlacements = 0x98, BlockId = 0xB8;
constexpr std::size_t FirstGroup = 24, GroupSize = 16, RecordSize = 24;
constexpr std::uint32_t MaxGroups = 1024, MaxGroupRecords = 65535;
// The grid, measured in Afghanistan: block ids run 64 a row; 128 m blocks from (-4096, -4096) put the
// placements on the loaded terrain. X and Z run 255 to the metre, so a block's half is 16,320: every one
// of 794 rock instances in the clone draws' instance buffers matched its record so (2026-10-25, Africa);
// read as 256 to the metre, only 2 did, the rest up to 34 cm nearer the block's centre than the game
// draws them, and the shadow rays met rocks the game shows elsewhere.
constexpr int GridColumns = 64;
constexpr float BlockSize = 128, GridOrigin = -4096, UnitsPerMetre = 255;

using Function = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t);
std::array<Function, std::size(Functions)> originals{};
std::uintptr_t imageBase{};

std::mutex liveMutex;
std::unordered_set<std::uintptr_t> brushes, blocks;

// A brush's plugin that draws a model, by plugin index: its model resource and scale range.
struct Plugin
{
    std::uintptr_t resource{};
    float scaleMin{}, scaleRange{};
};

// A block's placements as read when first seen, a group a plugin. By block, on the snapshot's thread.
struct Group
{
    std::uintptr_t resource{};
    std::vector<scene_description::Transform> transforms;
};
std::unordered_map<std::uintptr_t, std::vector<Group>> readBlocks;

// Diagnostics.
std::atomic<std::uint32_t> lastBrushes{}, lastBlocks{}, lastPlacements{}, unknownPlacements{};

template<std::size_t Index>
std::uintptr_t detour(std::uintptr_t self, std::uintptr_t argument)
{
    constexpr bool block = Index > BrushDestructor;
    constexpr bool destructor = Index == BrushDestructor || Index == BlockDestructor;
    if (destructor)
        try
        {
            std::lock_guard lock(liveMutex);
            (block ? blocks : brushes).erase(self);
        }
        catch (...)
        {
        }

    const auto result = originals[Index](self, argument);
    if (!destructor)
        try
        {
            std::lock_guard lock(liveMutex);
            (block ? blocks : brushes).insert(self);
        }
        catch (...)
        {
        }

    return result;
}

constexpr Function Detours[]{&detour<0>, &detour<1>, &detour<2>, &detour<3>, &detour<4>, &detour<5>, &detour<6>};
static_assert(std::size(Detours) == std::size(Functions));

// A placement's transform: the rotation's rows times the scale, the position last.
scene_description::Transform transformOf(float x, float y, float z, const float q[4], float scale)
{
    const float xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
    const float xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2], wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];
    return {(1 - 2 * (yy + zz)) * scale, 2 * (xy - wz) * scale, 2 * (xz + wy) * scale, x, 2 * (xy + wz) * scale, (1 - 2 * (xx + zz)) * scale,
        2 * (yz - wx) * scale, y, 2 * (xz - wy) * scale, 2 * (yz + wx) * scale, (1 - 2 * (xx + yy)) * scale, z};
}

std::vector<Plugin> pluginsOf(std::uintptr_t brush)
{
    std::vector<Plugin> plugins;
    const auto count = read<std::uint32_t>(brush + PluginCount);
    const auto links = read<std::uintptr_t>(brush + PluginLinks);
    if (!count || !links || *count > MaxPlugins) return plugins;

    std::vector<std::uintptr_t> linkArray(*count);
    if (!readRaw(linkArray.data(), *links, linkArray.size() * sizeof(std::uintptr_t))) return plugins;

    plugins.resize(*count);
    for (std::uint32_t i = 0; i < *count; ++i)
    {
        const auto plugin = linkArray[i] ? read<std::uintptr_t>(linkArray[i] + LinkPlugin) : std::nullopt;
        if (!plugin || std::none_of(std::begin(PluginVtables), std::end(PluginVtables), [&](auto vtable) { return isA(*plugin, imageBase + vtable); })) continue;

        const auto resource = read<std::uintptr_t>(*plugin + PluginModel);
        const auto scale = read<std::array<float, 2>>(*plugin + PluginScale);
        if (resource && scale) plugins[i] = {*resource, (*scale)[0], (*scale)[1] - (*scale)[0]};
    }

    return plugins;
}

using NamedPlugins = std::vector<std::pair<std::uintptr_t, std::vector<Plugin>>>;

// A block's placements, a group a plugin; none when the block or its brush cannot be read.
std::vector<Group> readBlock(std::uintptr_t block, const NamedPlugins& named)
{
    std::vector<Group> groups;
    const auto name = isA(block, imageBase + BlockVtable) ? read<std::uintptr_t>(block + BlockName) : std::nullopt;
    const auto brush = name ? std::find_if(named.begin(), named.end(), [&](const auto& n) { return n.first == *name; }) : named.end();
    const auto id = read<std::uint32_t>(block + BlockId);
    const auto data = read<std::uintptr_t>(block + BlockPlacements);
    const auto range = data ? read<std::array<std::uintptr_t, 2>>(*data) : std::nullopt;
    if (brush == named.end() || !id || !range || (*range)[1] < (*range)[0] + FirstGroup + GroupSize || ((*range)[1] - (*range)[0]) / GroupSize > MaxGroups)
        return groups;

    const auto blockId = static_cast<std::uint16_t>(*id);
    const float centreX = GridOrigin + (static_cast<float>(blockId % GridColumns) + 0.5f) * BlockSize;
    const float centreZ = GridOrigin + (static_cast<float>(blockId / GridColumns) + 0.5f) * BlockSize;
    const auto& plugins = brush->second;
    std::vector<std::uint8_t> records;
    for (auto group = (*range)[0] + FirstGroup; group + GroupSize <= (*range)[1]; group += GroupSize)
    {
        std::uint8_t header[GroupSize];
        if (!readRaw(header, group, sizeof(header)) || header[11] != 1) continue;

        std::uintptr_t at{};
        std::uint16_t count{};
        std::memcpy(&at, header, 8);
        std::memcpy(&count, header + 8, 2);
        const std::uint8_t groupPlugin = header[10];
        records.resize(std::size_t{count} * RecordSize);
        if (!count || count > MaxGroupRecords || !readRaw(records.data(), at, records.size())) continue;

        Group made;
        for (std::size_t i = 0; i < records.size(); i += RecordSize)
        {
            const auto* r = records.data() + i;
            std::uint16_t recordBlock{}, half[4]{};
            std::memcpy(&recordBlock, r + 16, 2);
            const std::uint8_t pluginIndex = r[18], scaleByte = r[19];
            if (recordBlock != blockId || pluginIndex != groupPlugin) continue;

            if (pluginIndex >= plugins.size() || !plugins[pluginIndex].resource)
            {
                ++unknownPlacements;
                continue;
            }

            float y{};
            std::int16_t x16{}, z16{};
            std::memcpy(&y, r, 4);
            std::memcpy(&x16, r + 4, 2);
            std::memcpy(&z16, r + 6, 2);
            std::memcpy(half, r + 8, 8);
            const float rotation[4]{halfToFloat(half[0]), halfToFloat(half[1]), halfToFloat(half[2]), halfToFloat(half[3])};
            const auto& plugin = plugins[pluginIndex];
            const float scale = plugin.scaleMin + plugin.scaleRange * (static_cast<float>(scaleByte) / 255.0f);
            made.resource = plugin.resource;
            made.transforms.push_back(transformOf(centreX + x16 / UnitsPerMetre, y, centreZ + z16 / UnitsPerMetre, rotation, scale));
        }

        if (!made.transforms.empty()) groups.push_back(std::move(made));
    }

    return groups;
}
}

std::string hook(std::uintptr_t base) noexcept
{
    imageBase = base;
    for (std::size_t i = 0; i < std::size(Functions); ++i)
        if (std::memcmp(reinterpret_cast<const void*>(base + Functions[i]), Starts[i], sizeof(Starts[i])))
            return " The object brush's constructors have unexpected code: its vegetation is left out of the ray-traced scene.";

    for (std::size_t i = 0; i < std::size(Functions); ++i)
    {
        if (const auto status = game_code::hookAt(reinterpret_cast<void*>(base + Functions[i]), Detours[i], originals[i]); status != MH_OK)
        {
            for (std::size_t j = 0; j < i; ++j) MH_RemoveHook(reinterpret_cast<void*>(base + Functions[j]));
            return std::string(" Hooking the object brush failed (") + MH_StatusToString(status) + "): its vegetation is left out of the ray-traced scene.";
        }
    }

    return " The object brush's constructors are hooked: the scene takes its vegetation.";
}

void collect(const std::function<void(std::uintptr_t resource, std::span<const scene_description::Transform> transforms)>& place)
{
    std::vector<std::uintptr_t> liveBrushes, liveBlocks;
    {
        std::lock_guard lock(liveMutex);
        liveBrushes.assign(brushes.begin(), brushes.end());
        liveBlocks.assign(blocks.begin(), blocks.end());
    }

    // Blocks gone are forgotten; the brushes' plugins are read only when a block is new.
    std::erase_if(readBlocks, [&](const auto& known) { return std::find(liveBlocks.begin(), liveBlocks.end(), known.first) == liveBlocks.end(); });
    NamedPlugins named;
    bool namedRead{};
    std::uint32_t placements{};
    for (const auto block : liveBlocks)
    {
        auto found = readBlocks.find(block);
        if (found == readBlocks.end())
        {
            // Each brush's plugins, by its name: its blocks name it.
            if (!namedRead)
            {
                namedRead = true;
                for (const auto brush : liveBrushes)
                    if (const auto name = isA(brush, imageBase + BrushVtable) ? read<std::uintptr_t>(brush + BrushName) : std::nullopt)
                        named.emplace_back(*name, pluginsOf(brush));
            }

            // A block whose placements or brush are not loaded yet is read again next time.
            auto groups = readBlock(block, named);
            if (groups.empty()) continue;

            found = readBlocks.emplace(block, std::move(groups)).first;
        }

        for (const auto& group : found->second)
        {
            place(group.resource, group.transforms);
            placements += static_cast<std::uint32_t>(group.transforms.size());
        }
    }

    lastBrushes = static_cast<std::uint32_t>(liveBrushes.size());
    lastBlocks = static_cast<std::uint32_t>(liveBlocks.size());
    lastPlacements = placements;
}

std::string status()
{
    return "the object brush: " + std::to_string(lastPlacements.load()) + " placements in " + std::to_string(lastBlocks.load()) + " blocks of "
        + std::to_string(lastBrushes.load()) + " brushes (" + std::to_string(unknownPlacements.load()) + " of plugins that draw no model left out)";
}
}
