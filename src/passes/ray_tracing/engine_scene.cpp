#include "passes/ray_tracing/engine_scene.hpp"

#include "device_check.hpp"
#include "game_code.hpp"
#include "game_memory.hpp"
#include "game_loop.hpp"
#include "passes/ray_tracing/engine_brush.hpp"
#include "passes/ray_tracing/scene_description.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace phantom_fx::engine_scene
{
namespace
{
using game_memory::read;
using game_memory::readRaw;
using scene_description::MaxBones;
using scene_description::ModelFile;
using scene_description::Source;

// The model visibility tests, called from the job threads with the visibility manager in rcx, once a
// view (main camera, shadow cascades) a frame; docs/DEVELOPMENT.md, "Scene objects".
constexpr std::uintptr_t VisibilityTestRvas[]{0x22C6F0, 0x22C8B0};
// mov [rsp+18h], rbx; push rbp, rsi, rdi, r12, r13, r14, r15; sub rsp, 0C40h
constexpr unsigned char VisibilityTestStart[]{0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
    0x57, 0x48, 0x81, 0xEC, 0x40, 0x0C, 0x00, 0x00};
constexpr std::uintptr_t ManagerVtable = 0x21065C8, GroupVtable = 0x2106720, ModelVtable = 0x20F4D90, ResourceVtable = 0x21027C0;
// The texture manager (a global, its vtable) keeps an array of 0x78-byte texture records, as many as
// its capacity (+0x28); a record's handle (+0x40, its vtable) has the texture's path hash at +0x20, its
// record's index at +0x34 and its resource at +0x10, whose first word points to an entry holding the
// ID3D11Texture2D at +0x10. The array's start is found from its streaming list (+0x10 to +0x18,
// 24-byte nodes whose first word points 0x10 into a record): a listed record less its index's records.
// The list leaves a few loaded textures out, so the array is walked, not the list.
constexpr std::uintptr_t TextureManagerGlobal = 0x2B79B18, TextureManagerVtable = 0x21046E8, TextureVtable = 0x2104710;
constexpr std::size_t TextureNodes = 0x10, TextureNodeSize = 24, NodeToRecord = 0x10, TextureCapacity = 0x28, TextureRecordSize = 0x78, RecordTexture = 0x40,
                      TexturePath = 0x20, TextureIndex = 0x34, TextureResource = 0x10, EntryTexture = 0x10;
constexpr std::uint32_t MaxTextureRecords = 1 << 16;

// The manager's record chunks: 24-byte entries (chunk, handle tag, element size); the tag's low 32 bits
// hold 3 in the top nibble and the chunk's number in bits 16 to 23, its high 32 bits other data, set
// from chunk 11 of the largest manager on (read as 64 bits, the walk stopped there and left 60% of
// Africa's placements out). A record in use holds the manager plus RecordInUse, and its owner.
constexpr std::size_t ChunkTable = 0x88, RecordInUse = 0x100;
constexpr std::size_t RecordSize = 0x30, RecordsPerChunk = 256, RecordUse = 0x20, RecordOwner = 0x28;
// An instance group: its model, and the holder of its world matrices (begin, end).
constexpr std::size_t GroupModel = 0x40, GroupMatrices = 0x60, MatricesBegin = 0x20, MatricesEnd = 0x28;
// A model: its world matrix (when an instance itself) and resource, which has the file's path hash,
// the file in memory and its size.
constexpr std::size_t ModelMatrix = 0x40, ModelResource = 0x118, ResourceHash = 0x30, ResourceFile = 0x60, ResourceFileSize = 0x68;
// A model's passes it is hidden from, a bit a pass: the draw lists leave it out of a pass whose bit is
// set (0x25BE00). The game's model handles show and hide it (0x2165F0: 0 shown, all set hidden).
// Levels keep hidden copies of rocks and cliffs, in the visibility manager too.
constexpr std::size_t ModelHiddenPasses = 0x1A4;
constexpr std::uint32_t HiddenFromEveryPass = 0xFFFFFFFF;
// A model's fade, 0 (gone) to 1 (as drawn); the constructor (0x1C77B0) sets 1. Watched 2026-10-27 in
// Mother Base: models faded in from near 0 to 1 in about two seconds, and some near the camera dipped to
// 0.83 and back. Held at 0 there: of 257 shown rigid models only a building's outer shell (model
// 84a2063b554c5d57, mesh 0, main view only), which no draw of a frame held, and one whose meshes the
// scene leaves out anyway; and 23 hidden ones. Taken, the shell lay 13 cm off a wall the game drew, and
// the wall's global illumination rays met its inside: the wall went black up close, where they no
// longer skip it as a copy of the wall.
constexpr std::size_t ModelFade = 0x1B8;
// A model's layer flags, which a few draw-list builders test against a pass's (0x25BE00). Bit 0x20 marks
// the silhouette of a soldier the player marked (a copy of its skeleton with glass-flagged meshes, its
// root bone's matrix not a pose): the game draws it in the main view only, through walls (measured
// draw by draw, 2026-10-23: the only two such models, never in a shadow pass). Posed as a character,
// its root-weighted vertices streaked from the soldier to the player.
constexpr std::size_t ModelLayer = 0x1AC;
constexpr std::uint32_t MarkerSilhouetteLayer = 0x20;
// Every model, characters too, is on the object registry's model list (docs/DEVELOPMENT.md, "Every
// model"): the global pointing to the registry, its table of list headers by type number (a header:
// the last object, then the first), the global holding the models' type number, and the link from
// one object to the next.
constexpr std::uintptr_t RegistryGlobal = 0x2BA04A8, RegistryVtable = 0x20F08C8, ModelTypeGlobal = 0x29E51B4;
constexpr std::size_t RegistryLists = 0x1E8, ListFirst = 8, ObjectNext = 0x10;
constexpr std::uint32_t MaxTypes = 256, MaxListedModels = 1 << 16;
// A skinned model (docs/DEVELOPMENT.md, "Characters"): its bone count (16 bits: a flag above it is set
// on animals' and some characters' models, the dog's 0x1004E for 78 bones), the object whose +0x78 holds
// its skinning palette, and its mesh groups' count and state, a byte each: drawn while none of
// HiddenGroupBits is set (measured against a soldier's draws: 13 groups drawn, 33 not). Read for a rigid
// model too (hiddenGroupsOf): the game never drew 17 of the 29 meshes the scene took of the hospital
// entrance, rubble slabs that floated over its porch in the rays and cast blots on it (the lamp probe,
// 2026-10-26).
constexpr std::size_t ModelBoneCount = 0xF8, ModelSkin = 0x188, SkinPalette = 0x78, ModelGroupCount = 0x1E8, ModelGroupStates = 0x178;
constexpr std::uint8_t HiddenGroupBits = 0x33;
constexpr std::uint32_t MaxChunks = 64, MaxManagers = 8;
constexpr std::uint64_t MaxGroupInstances = 4096, MaxFileBytes = 64ull << 20;
// A model file that does not read as one (still streaming in, or not an FMDL) is looked at again for
// this many snapshots before it is given up.
constexpr std::uint32_t UnreadableRetries = 600;
// New model files read a take at most; the rest wait for the next takes.
constexpr std::uint64_t ParseBytesPerTake = 8ull << 20;

using VisibilityTest = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t);
std::array<VisibilityTest, 2> originals{};
std::uintptr_t base{};
std::atomic<bool> active{};

// Taking a snapshot: one job thread a frame.
std::mutex snapshotMutex;
std::atomic<std::uint64_t> lastSnapshotFrame{};
// The visibility managers the tests ran with (noteManager), a slot each.
std::array<std::atomic<std::uintptr_t>, MaxManagers> seenManagers{};
// Models by path hash: rigid or skinned (its file queued or read), or unreadable (given up), with the
// source that asked for it first, whose roles its meshes take; and the snapshots each unreadable file
// has been tried in.
struct KnownModel
{
    ModelFile file{};
    Source source{};
    bool isShared{}; // Diagnostics: counted in sharedFiles
};
std::unordered_map<std::uint64_t, KnownModel> knownModels;
std::unordered_map<std::uint64_t, std::uint32_t> unreadableTries;
std::vector<std::uint8_t> recordBuffer(RecordSize * RecordsPerChunk);
std::vector<float> matrixBuffer;

struct PendingFile
{
    std::uint64_t hash{};
    std::vector<std::uint8_t> bytes;
    Source source{};
};

// What the snapshots publish for the render thread.
std::mutex publishMutex;
std::vector<scene_description::ModelInstance> latestInstances;
std::vector<scene_description::CharacterInstance> latestCharacters;
bool latestFresh{};

// The skinned models parsed, for their meshes' index ranges and bone groups (take); render thread.
std::unordered_map<std::uint64_t, std::shared_ptr<const scene_description::Model>> skinnedModels;

// Diagnostics: where the bones came from.
std::atomic<std::uint64_t> posesCompared{}, posesChanged{}, drawsNoted{}, drawsPlaced{}, meshesPosed{};
std::vector<PendingFile> pendingFiles;
// The terrain inputs last noted, and the game frame they were noted in.
std::mutex terrainMutex;
std::optional<TerrainInputs> terrainInputs;
std::atomic<std::uint64_t> terrainFrame{};

// Diagnostics.
std::atomic<std::uint64_t> snapshots{}, filesRead{}, skinnedFiles{}, unreadableFiles{}, modelsParsed{}, parseFailures{}, shadowlessMeshes{};
// Files the object brush and another source both asked for: read once, their meshes have the roles of the
// source that asked first.
std::atomic<std::uint64_t> sharedFiles{};
std::atomic<std::uint32_t> lastInstances{}, lastOwners{}, lastOtherOwners{}, lastLeftOut{}, lastHidden{}, lastFadedOut{}, lastHidingGroups{}, lastListedOnly{},
    lastCharacters{}, lastListedModels{};
// The model list was found in the last snapshot: the characters come from it.
std::atomic<bool> modelListFound{};

// Takes a reference to a COM object the game holds; false when it is gone.
bool hold(IUnknown* object) noexcept
{
    __try
    {
        object->AddRef();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool isA(std::uintptr_t object, std::uintptr_t vtableRva) noexcept
{
    return game_memory::isA(object, base + vtableRva);
}

// A stored world matrix (4x4, column-major, translation in the last column) as the scene's rows. The
// bottom row is (0 0 0 1) give or take rounding: a weapon held in a hand, its matrix made from the
// hand's bone, had 0.99999988 for the 1.
std::optional<scene_description::Transform> transformOf(const float* m) noexcept
{
    constexpr float Rounding = 1e-3f;
    if (std::abs(m[3]) > Rounding || std::abs(m[7]) > Rounding || std::abs(m[11]) > Rounding || std::abs(m[15] - 1) > Rounding) return std::nullopt;

    scene_description::Transform rows{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
        {
            const float value = m[column * 4 + row];
            if (!std::isfinite(value) || std::abs(value) > 1e6f) return std::nullopt;

            rows[row * 4 + column] = value;
        }

    return rows;
}

// A model resource's path hash when its model file is of the kind `wanted`; its file is queued the first
// time it is seen, from `source` (scene_description::describeModel). On the job thread taking the snapshot.
std::optional<std::uint64_t> modelResource(std::uintptr_t resource, ModelFile wanted, Source source)
{
    if (!isA(resource, ResourceVtable)) return std::nullopt;

    const auto hash = read<std::uint64_t>(resource + ResourceHash);
    if (!hash) return std::nullopt;

    if (const auto found = knownModels.find(*hash); found != knownModels.end())
    {
        auto& known = found->second;
        if (!known.isShared && (known.source == Source::ObjectBrush) != (source == Source::ObjectBrush))
        {
            known.isShared = true;
            ++sharedFiles;
        }

        return known.file == wanted ? hash : std::nullopt;
    }

    // A file not loaded yet is looked at again next frame.
    const auto fileAt = read<std::uintptr_t>(resource + ResourceFile);
    const auto size = read<std::uint32_t>(resource + ResourceFileSize);
    if (!fileAt || !*fileAt || !size || *size < 0x40) return std::nullopt;

    PendingFile file{*hash, {}, source};
    if (*size <= MaxFileBytes)
    {
        file.bytes.resize(*size);
        if (!readRaw(file.bytes.data(), *fileAt, file.bytes.size())) return std::nullopt;
    }

    const auto kind = scene_description::fileKindOf(file.bytes);
    if (kind == ModelFile::Unreadable)
    {
        // Possibly still streaming in: given up only after many snapshots.
        if (++unreadableTries[*hash] < UnreadableRetries) return std::nullopt;

        unreadableTries.erase(*hash);
        knownModels[*hash] = {kind, source};
        ++unreadableFiles;
        return std::nullopt;
    }

    unreadableTries.erase(*hash);
    knownModels[*hash] = {kind, source};
    ++(kind == ModelFile::Skinned ? skinnedFiles : filesRead);
    {
        std::lock_guard lock(publishMutex);
        pendingFiles.push_back(std::move(file));
    }

    return kind == wanted ? hash : std::nullopt;
}

// A rigid model's mesh groups the game hides, as a character's (ModelGroupStates), a bit each for the
// first 64; 0 when it shows them all or they cannot be read.
std::uint64_t hiddenGroupsOf(std::uintptr_t model)
{
    const auto groups = read<std::uint16_t>(model + ModelGroupCount);
    const auto states = read<std::uintptr_t>(model + ModelGroupStates);
    if (!groups || !*groups || !states || !*states) return 0;

    std::array<std::uint8_t, 64> bytes{};
    const auto count = std::min<std::size_t>(*groups, bytes.size());
    if (!readRaw(bytes.data(), *states, count)) return 0;

    std::uint64_t hidden{};
    for (std::size_t g = 0; g < count; ++g)
        if (bytes[g] & HiddenGroupBits) hidden |= 1ull << g;

    return hidden;
}

bool fadedOut(std::uintptr_t model)
{
    return read<float>(model + ModelFade) == 0.0f;
}

std::optional<std::uint64_t> rigidModel(std::uintptr_t model)
{
    if (!isA(model, ModelVtable)) return std::nullopt;

    const auto resource = read<std::uintptr_t>(model + ModelResource);
    return resource ? modelResource(*resource, ModelFile::Rigid, Source::VisibilityManager) : std::nullopt;
}

// The game's characters: every skinned model on the object registry's model list the game shows,
// in its pose this frame (docs/DEVELOPMENT.md, "Every model", "Characters"), each rigid model on the
// list it shows handed to `listedRigid`; false without the list.
bool collectCharacters(std::vector<scene_description::CharacterInstance>& characters, std::uint32_t& listed, const std::function<void(std::uintptr_t model)>& listedRigid)
{
    const auto registry = read<std::uintptr_t>(base + RegistryGlobal);
    const auto type = read<std::uint32_t>(base + ModelTypeGlobal);
    if (!registry || !type || *type >= MaxTypes || !isA(*registry, RegistryVtable)) return false;

    const auto list = read<std::uintptr_t>(*registry + RegistryLists + 8ull * *type);
    auto node = list && *list ? read<std::uintptr_t>(*list + ListFirst) : std::nullopt;
    if (!node) return false;

    for (listed = 0; *node && listed < MaxListedModels; ++listed)
    {
        const auto model = *node;
        if (!isA(model, ModelVtable)) break;

        node = read<std::uintptr_t>(model + ObjectNext);
        if (!node) break;
        if (read<std::uint32_t>(model + ModelHiddenPasses) == HiddenFromEveryPass) continue;
        if (read<std::uint32_t>(model + ModelLayer).value_or(0) & MarkerSilhouetteLayer) continue;

        // The file is queued last: a model that is no character (most of the list's are rigid) has its
        // file read for nothing, again each time the renderer lets go of what it never placed.
        const auto bones = read<std::uint16_t>(model + ModelBoneCount);
        const auto skin = read<std::uintptr_t>(model + ModelSkin);
        const auto palette = skin && *skin ? read<std::uintptr_t>(*skin + SkinPalette) : std::nullopt;
        const auto groups = read<std::uint16_t>(model + ModelGroupCount);
        const auto states = read<std::uintptr_t>(model + ModelGroupStates);
        float matrix[16]{};
        if (!bones || !*bones || *bones > MaxBones || !palette || !*palette || !groups || !states || !readRaw(matrix, model + ModelMatrix, sizeof(matrix)))
        {
            listedRigid(model);
            continue;
        }

        const auto transform = transformOf(matrix);
        const auto resource = transform ? read<std::uintptr_t>(model + ModelResource) : std::nullopt;
        const auto hash = resource ? modelResource(*resource, ModelFile::Skinned, Source::ModelList) : std::nullopt;
        if (!hash) continue;

        scene_description::CharacterInstance character{model, *hash, *transform, std::vector<float>(std::size_t{*bones} * 12), {}};
        std::vector<std::uint8_t> groupStates(*groups);
        if (!readRaw(character.palette.data(), *palette, character.palette.size() * sizeof(float))
            || (*groups && (!*states || !readRaw(groupStates.data(), *states, groupStates.size()))))
            continue;

        character.shownGroups.resize(*groups);
        for (std::size_t g = 0; g < groupStates.size(); ++g) character.shownGroups[g] = (groupStates[g] & HiddenGroupBits) == 0;
        character.groupStates = std::move(groupStates);
        characters.push_back(std::move(character));
    }

    return true;
}

// Every record in use of the managers seen, their owners' instances, published; with snapshotMutex.
void snapshot()
{
    std::vector<std::uintptr_t> owners;
    for (auto& slot : seenManagers)
    {
        const auto manager = slot.load(std::memory_order_relaxed);
        if (!manager) continue;

        // A manager gone frees its slot for the one that replaces it.
        if (!isA(manager, ManagerVtable))
        {
            auto expected = manager;
            slot.compare_exchange_strong(expected, 0);
            continue;
        }

        const auto table = read<std::uintptr_t>(manager + ChunkTable);
        if (!table) continue;

        for (std::uint32_t chunk = 0; chunk < MaxChunks; ++chunk)
        {
            const auto records = read<std::uintptr_t>(*table + 24 * chunk);
            const auto tag = read<std::uint32_t>(*table + 24 * chunk + 8);
            const auto size = read<std::uint16_t>(*table + 24 * chunk + 16);
            if (!records || !*records || !tag || (*tag >> 28) != 3 || ((*tag >> 16) & 0xFF) != chunk || !size || *size != RecordSize) break;
            if (!readRaw(recordBuffer.data(), *records, recordBuffer.size())) continue;

            for (std::size_t i = 0; i < RecordsPerChunk; ++i)
            {
                std::uintptr_t use{}, owner{};
                std::memcpy(&use, recordBuffer.data() + i * RecordSize + RecordUse, sizeof(use));
                std::memcpy(&owner, recordBuffer.data() + i * RecordSize + RecordOwner, sizeof(owner));
                if (use == manager + RecordInUse && owner) owners.push_back(owner);
            }
        }
    }

    std::sort(owners.begin(), owners.end());
    owners.erase(std::unique(owners.begin(), owners.end()), owners.end());

    std::vector<scene_description::ModelInstance> instances;
    std::uint32_t others{}, leftOut{}, hidden{}, fadedOutModels{}, hidingGroups{}, listedOnly{};
    const auto place = [&](std::uint64_t hash, const float* matrix, std::uint64_t hiddenGroups) {
        const auto transform = transformOf(matrix);
        if (!transform)
        {
            ++leftOut;
            return;
        }

        if (hiddenGroups) ++hidingGroups;
        instances.push_back({hash, *transform, hiddenGroups});
    };

    for (const auto owner : owners)
    {
        const auto vtable = read<std::uintptr_t>(owner);
        if (!vtable) continue;

        if (*vtable == base + GroupVtable)
        {
            const auto model = read<std::uintptr_t>(owner + GroupModel);
            const auto hash = model ? rigidModel(*model) : std::nullopt;
            const auto holder = read<std::uintptr_t>(owner + GroupMatrices);
            const auto begin = holder ? read<std::uintptr_t>(*holder + MatricesBegin) : std::nullopt;
            const auto end = holder ? read<std::uintptr_t>(*holder + MatricesEnd) : std::nullopt;
            if (!hash || !begin || !end || *end < *begin || (*end - *begin) % 64 || (*end - *begin) / 64 > MaxGroupInstances)
            {
                ++leftOut;
                continue;
            }

            matrixBuffer.resize((*end - *begin) / 4);
            if (!readRaw(matrixBuffer.data(), *begin, *end - *begin)) continue;

            const auto hiddenGroups = hiddenGroupsOf(*model);
            for (std::size_t i = 0; i < matrixBuffer.size(); i += 16) place(*hash, matrixBuffer.data() + i, hiddenGroups);
        }
        else if (*vtable == base + ModelVtable)
        {
            if (read<std::uint32_t>(owner + ModelHiddenPasses) == HiddenFromEveryPass)
            {
                ++hidden;
                continue;
            }

            if (fadedOut(owner))
            {
                ++fadedOutModels;
                continue;
            }

            float matrix[16]{};
            const auto hash = rigidModel(owner);
            if (hash && readRaw(matrix, owner + ModelMatrix, sizeof(matrix))) place(*hash, matrix, hiddenGroupsOf(owner));
            else ++leftOut;
        }
        else
            ++others;
    }

    // The object brush's vegetation, a plugin's placements in a block at a time.
    engine_brush::collect([&](std::uintptr_t resource, std::span<const scene_description::Transform> transforms) {
        if (const auto hash = modelResource(resource, ModelFile::Rigid, Source::ObjectBrush))
            for (const auto& transform : transforms) instances.push_back({*hash, transform, 0});
    });

    // A rigid model no visibility manager holds; those it holds are placed above.
    const auto listedRigid = [&](std::uintptr_t model) {
        if (std::binary_search(owners.begin(), owners.end(), model)) return;

        float matrix[16]{};
        const auto hash = rigidModel(model);
        if (!hash || !readRaw(matrix, model + ModelMatrix, sizeof(matrix))) return;

        ++listedOnly;
        place(*hash, matrix, hiddenGroupsOf(model));
    };

    std::vector<scene_description::CharacterInstance> characters;
    std::uint32_t listed{};
    modelListFound = collectCharacters(characters, listed, listedRigid);

    ++snapshots;
    lastInstances = static_cast<std::uint32_t>(instances.size());
    lastOwners = static_cast<std::uint32_t>(owners.size());
    lastOtherOwners = others;
    lastLeftOut = leftOut;
    lastHidden = hidden;
    lastFadedOut = fadedOutModels;
    lastHidingGroups = hidingGroups;
    lastListedOnly = listedOnly;
    lastCharacters = static_cast<std::uint32_t>(characters.size());
    lastListedModels = listed;
    std::lock_guard lock(publishMutex);
    latestInstances = std::move(instances);
    latestCharacters = std::move(characters);
    latestFresh = true;
}

// Registers a manager in the first free slot unless it has one; any thread.
void noteManager(std::uintptr_t manager) noexcept
{
    for (auto& slot : seenManagers)
    {
        auto current = slot.load(std::memory_order_relaxed);
        if (current == manager) return;
        if (!current && (slot.compare_exchange_strong(current, manager) || current == manager)) return;
    }
}

// Before the game's visibility test: every manager it runs with is noted, and the first test a game
// frame takes the snapshot.
void beforeVisibilityTest(std::uintptr_t manager) noexcept
{
    if (!active.load(std::memory_order_relaxed)) return;

    // Noted on every test: a manager whose tests never come first in a frame was missed before.
    if (isA(manager, ManagerVtable)) noteManager(manager);

    // Without the game loop's count, about once a 60 Hz frame.
    const std::uint64_t frame = game_loop::currentFrame() ? game_loop::currentFrame() : GetTickCount64() / 16;
    if (lastSnapshotFrame.load(std::memory_order_relaxed) == frame) return;

    std::unique_lock lock(snapshotMutex, std::try_to_lock);
    if (!lock.owns_lock() || lastSnapshotFrame.load(std::memory_order_relaxed) == frame) return;

    try
    {
        lastSnapshotFrame = frame;
        snapshot();
    }
    catch (...)
    {
    }
}

template<std::size_t Index>
void visibilityTest(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6, std::uintptr_t a7,
    std::uintptr_t a8, std::uintptr_t a9, std::uintptr_t a10)
{
    beforeVisibilityTest(a1);
    originals[Index](a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

constexpr VisibilityTest Detours[]{&visibilityTest<0>, &visibilityTest<1>};
}

std::string hook() noexcept
{
    if (!isSupportedGameBuild()) return "Unknown game build: the ray-traced scene has none of the game's models.";

    base = reinterpret_cast<std::uintptr_t>(game_code::image());
    for (const auto rva : VisibilityTestRvas)
        if (std::memcmp(reinterpret_cast<const void*>(base + rva), VisibilityTestStart, sizeof(VisibilityTestStart)))
            return "The visibility tests have unexpected code: the ray-traced scene has none of the game's models.";

    for (std::size_t i = 0; i < std::size(VisibilityTestRvas); ++i)
    {
        if (const auto status = game_code::hookAt(reinterpret_cast<void*>(base + VisibilityTestRvas[i]), Detours[i], originals[i]); status != MH_OK)
        {
            for (std::size_t j = 0; j < i; ++j) MH_RemoveHook(reinterpret_cast<void*>(base + VisibilityTestRvas[j]));
            return std::string("Hooking the visibility tests failed (") + MH_StatusToString(status) + "): the ray-traced scene has none of the game's models.";
        }
    }

    return "The visibility tests are hooked: the ray-traced scene takes the game's loaded models from its visibility manager." + engine_brush::hook(base);
}

void setActive(bool on) noexcept
{
    active = on;
    if (on) return;

    // Off, the renderer lets its scene go: the model files are read again when next on.
    modelListFound = false;

    try
    {
        std::scoped_lock lock(snapshotMutex, publishMutex);
        knownModels.clear();
        unreadableTries.clear();
        pendingFiles.clear();
        latestInstances.clear();
        latestCharacters.clear();
        latestFresh = false;
        lastSnapshotFrame = 0;
    }
    catch (...)
    {
    }
}

void take(Snapshot& snapshot, const std::vector<gi_inputs::DrawnBones>& drawn)
{
    std::vector<PendingFile> files;
    {
        std::lock_guard lock(publishMutex);
        snapshot.instancesUpdated = latestFresh;
        if (latestFresh)
        {
            snapshot.instances = std::move(latestInstances);
            snapshot.characters = std::move(latestCharacters);
        }

        latestFresh = false;

        std::uint64_t bytes{};
        std::size_t taken{};
        while (taken < pendingFiles.size() && bytes < ParseBytesPerTake) bytes += pendingFiles[taken++].bytes.size();
        files.assign(std::make_move_iterator(pendingFiles.begin()), std::make_move_iterator(pendingFiles.begin() + taken));
        pendingFiles.erase(pendingFiles.begin(), pendingFiles.begin() + taken);
    }

    // The snapshot reads the bones at the frame's first visibility test, before the game animates them
    // for the frame it draws (measured 2026-10-24: in the helicopter, a lamp's shadow of Snake trailed his
    // moving arms and face). The bones the frame's draws had replace them: each draw's mesh found by its
    // model's place and its index range, its g_vsBone slots written through its bone group. The bones no
    // draw had are read again now, at the kick, which races the game's next frame.
    if (!snapshot.characters.empty())
    {
        std::vector<float> now;
        for (auto& character : snapshot.characters)
        {
            const auto skin = read<std::uintptr_t>(character.id + ModelSkin);
            const auto palette = skin && *skin ? read<std::uintptr_t>(*skin + SkinPalette) : std::nullopt;
            now.resize(character.palette.size());
            if (!palette || !*palette || !readRaw(now.data(), *palette, now.size() * sizeof(float))) continue;

            ++posesCompared;
            if (std::memcmp(now.data(), character.palette.data(), now.size() * sizeof(float)) == 0) continue;

            ++posesChanged;
            character.palette = now;
        }

        // Within 2 cm: the place is a product of the game's float matrices.
        constexpr float SamePlace = 0.02f;
        drawsNoted += drawn.size();
        for (const auto& mesh : drawn)
            for (auto& character : snapshot.characters)
            {
                const auto& t = character.transform;
                if (std::abs(t[3] - mesh.position[0]) > SamePlace || std::abs(t[7] - mesh.position[1]) > SamePlace
                    || std::abs(t[11] - mesh.position[2]) > SamePlace)
                    continue;

                const auto model = skinnedModels.find(character.hash);
                if (model == skinnedModels.end()) continue;

                const auto& parts = model->second->parts;
                const auto part = std::find_if(parts.begin(), parts.end(),
                    [&](const auto& candidate) { return candidate.firstIndex == mesh.firstIndex && candidate.indexCount == mesh.indexCount; });
                if (part == parts.end()) continue;

                ++drawsPlaced;
                character.drawnParts.resize(parts.size());
                character.drawnParts[static_cast<std::size_t>(part - parts.begin())] = true;
                for (std::size_t slot = 0; slot < part->boneGroup.size(); ++slot)
                {
                    const std::size_t bone = part->boneGroup[slot];
                    if ((bone + 1) * 12 > character.palette.size()) continue;

                    std::copy_n(mesh.bones.begin() + slot * 12, 12, character.palette.begin() + bone * 12);
                }

                ++meshesPosed;
                break;
            }
    }

    for (const auto& file : files)
    {
        auto parsed = scene_description::describeModel(file.hash, file.bytes, file.source);
        if (!parsed)
        {
            ++parseFailures;
            continue;
        }

        ++modelsParsed;
        shadowlessMeshes += parsed->dropped.size();
        if (parsed->bones) skinnedModels[parsed->hash] = parsed;
        snapshot.newModels.push_back(std::move(parsed));
    }
}

void forget(const std::vector<std::uint64_t>& hashes)
{
    if (hashes.empty()) return;

    std::lock_guard lock(snapshotMutex);
    for (const auto hash : hashes)
    {
        knownModels.erase(hash);
        skinnedModels.erase(hash);
    }
}

void noteTerrain(ID3D11DeviceContext* context, const game_constants::ObjectConstants& object, const game_constants::TerrainMaterialConstants& material) noexcept
{
    const std::uint64_t frame = game_loop::currentFrame() ? game_loop::currentFrame() : GetTickCount64() / 16;
    if (terrainFrame.exchange(frame, std::memory_order_relaxed) == frame) return;

    try
    {
        TerrainInputs inputs;
        ID3D11ShaderResourceView* views[2]{};
        context->VSGetShaderResources(0, 2, views);
        inputs.pages.Attach(views[0]);
        inputs.heights.Attach(views[1]);
        static_assert(sizeof(inputs.converter) == sizeof(material.pageTableConverter));
        std::memcpy(inputs.converter.data(), material.pageTableConverter, sizeof(inputs.converter));
        std::memcpy(inputs.scaleOffset.data(), &object.localParam[1], sizeof(inputs.scaleOffset));
        if (!inputs.pages || !inputs.heights) return;

        std::lock_guard lock(terrainMutex);
        terrainInputs = std::move(inputs);
    }
    catch (...)
    {
    }
}

std::optional<TerrainInputs> takeTerrain()
{
    std::lock_guard lock(terrainMutex);
    return std::exchange(terrainInputs, std::nullopt);
}

std::vector<LoadedTexture> loadedTextures(const std::vector<std::uint64_t>& paths)
{
    std::vector<LoadedTexture> found;
    if (!base || paths.empty()) return found;

    try
    {
        const auto manager = read<std::uintptr_t>(base + TextureManagerGlobal);
        if (!manager || !isA(*manager, TextureManagerVtable)) return found;

        const auto capacity = read<std::uint32_t>(*manager + TextureCapacity);
        const auto begin = read<std::uintptr_t>(*manager + TextureNodes), end = read<std::uintptr_t>(*manager + TextureNodes + 8);
        if (!capacity || *capacity > MaxTextureRecords || !begin || !end || *end < *begin || (*end - *begin) / TextureNodeSize > *capacity) return found;

        // The record array's start, from the first listed record whose handle gives its index.
        std::optional<std::uintptr_t> records;
        for (auto node = *begin; node < *end && !records; node += TextureNodeSize)
        {
            const auto record = read<std::uintptr_t>(node);
            if (!record || !*record || (*record >= *begin && *record < *begin + std::uintptr_t{*capacity} * TextureNodeSize)) continue;

            const auto handle = read<std::uintptr_t>(*record - NodeToRecord + RecordTexture);
            const auto index = handle && isA(*handle, TextureVtable) ? read<std::uint32_t>(*handle + TextureIndex) : std::nullopt;
            if (index && *index < *capacity) records = *record - NodeToRecord - std::uintptr_t{*index} * TextureRecordSize;
        }

        std::vector<std::uint8_t> array(std::size_t{*capacity} * TextureRecordSize);
        if (!records || !readRaw(array.data(), *records, array.size())) return found;

        const std::unordered_set<std::uint64_t> wanted(paths.begin(), paths.end());
        for (std::size_t at = 0; at + TextureRecordSize <= array.size(); at += TextureRecordSize)
        {
            std::uintptr_t handle{};
            std::memcpy(&handle, array.data() + at + RecordTexture, sizeof(handle));
            if (!handle || !isA(handle, TextureVtable)) continue;

            const auto path = read<std::uint64_t>(handle + TexturePath);
            if (!path || !wanted.contains(*path)) continue;

            // Held, then looked up again: the game swaps a texture for a fuller one as its mips stream in,
            // and one swapped while it was taken is left for the next call.
            const auto texture = [&]() -> std::optional<std::uintptr_t> {
                const auto resource = read<std::uintptr_t>(handle + TextureResource);
                const auto entry = resource ? read<std::uintptr_t>(*resource) : std::nullopt;
                return entry ? read<std::uintptr_t>(*entry + EntryTexture) : std::nullopt;
            };
            const auto object = texture();
            if (!object || !*object || !hold(reinterpret_cast<IUnknown*>(*object))) continue;

            Microsoft::WRL::ComPtr<IUnknown> held;
            held.Attach(reinterpret_cast<IUnknown*>(*object));
            LoadedTexture loaded{*path};
            if (texture() == object && SUCCEEDED(held.As(&loaded.texture))) found.push_back(std::move(loaded));
        }
    }
    catch (...)
    {
    }

    return found;
}

std::string poseTiming()
{
    const auto compared = posesCompared.exchange(0), changed = posesChanged.exchange(0);
    const auto noted = drawsNoted.exchange(0), placed = drawsPlaced.exchange(0), posed = meshesPosed.exchange(0);
    return "characters' bones: " + std::to_string(changed) + " of " + std::to_string(compared) + " palettes moved on since the snapshot; "
        + std::to_string(noted) + " skinned draws, " + std::to_string(placed) + " found their character and mesh, " + std::to_string(posed)
        + " meshes posed as drawn";
}

std::string status()
{
    if (!snapshots.load()) return "the game's scene: no snapshot yet";

    return "the game's scene: " + std::to_string(lastInstances.load()) + " instances from " + std::to_string(lastOwners.load()) + " owners ("
        + std::to_string(lastOtherOwners.load()) + " of other kinds, " + std::to_string(lastLeftOut.load()) + " left out, "
        + std::to_string(lastHidden.load()) + " models the game hides, " + std::to_string(lastFadedOut.load()) + " faded out, "
        + std::to_string(lastHidingGroups.load())
        + " placed with mesh groups hidden), " + std::to_string(lastListedOnly.load()) + " rigid models from the model list alone, " + std::to_string(lastCharacters.load())
        + " characters of "
        + std::to_string(lastListedModels.load()) + " models listed" + (modelListFound.load() ? "" : " (no model list)") + ", "
        + std::to_string(snapshots.load()) + " snapshots; model files: " + std::to_string(filesRead.load()) + " rigid, "
        + std::to_string(skinnedFiles.load()) + " skinned, " + std::to_string(unreadableFiles.load()) + " unreadable, "
        + std::to_string(modelsParsed.load()) + " parsed, "
        + std::to_string(parseFailures.load()) + " failed, " + std::to_string(sharedFiles.load())
        + " asked for by the object brush and another source (roles from the first); " + std::to_string(shadowlessMeshes.load())
        + " meshes left out (no shadow or see-through, or a character's shadow stand-in); " + engine_brush::status();
}
}
