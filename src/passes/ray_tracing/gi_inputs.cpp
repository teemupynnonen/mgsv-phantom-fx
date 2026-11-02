// The ray tracing's inputs from the game's draws; gi_inputs.hpp describes them.
#include "passes/ray_tracing/gi_inputs.hpp"

#include "clock.hpp"
#include "command_list_notes.hpp"
#include "constant_capture.hpp"
#include "log.hpp"
#include "passes/ray_tracing/engine_scene.hpp"

#include <Windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace phantom_fx::gi_inputs
{
namespace
{
using Microsoft::WRL::ComPtr;
using namespace game_constants;
using game_shaders::VertexShader;

// The skinned meshes noted between kicks at most.
constexpr std::size_t MaxDrawnBones = 2048;

std::atomic<bool> active{};

// What the vertex shader the thread's context bound last draws.
thread_local ID3D11DeviceContext* boundContext{};
thread_local VertexShader boundKind{};

// The camera the rays trace from: a list's first main-camera draw's.
struct Notes
{
    std::optional<SceneConstants> camera;

    void merge(Notes later)
    {
        if (!camera) camera = later.camera;
    }
};

// The camera by deferred context while recorded, by command list once finished, and that of the lists
// executed since the last kick.
CommandListNotes<Notes> pending;
std::mutex executedMutex;
std::optional<SceneConstants> executed;

// Diagnostics: the draw hook's time in performance counter ticks, and the draws, since takeWork.
std::atomic<std::int64_t> hookTicks{};
std::atomic<std::uint64_t> workDraws{};

// Adds the time from its construction to its destruction to hookTicks; nothing without the diagnostic
// log, as the game's threads would share the counter on every draw.
struct HookTimer
{
    LARGE_INTEGER start{};

    HookTimer()
    {
        if constexpr (DiagnosticLog) QueryPerformanceCounter(&start);
    }

    ~HookTimer()
    {
        if constexpr (DiagnosticLog)
        {
            LARGE_INTEGER end{};
            QueryPerformanceCounter(&end);
            hookTicks += end.QuadPart - start.QuadPart;
        }
    }
};

// The draws' skinned meshes since the last kick (takeDrawnBones).
std::mutex bonesMutex;
std::vector<DrawnBones> drawnBones;

#if PHANTOM_FX_DEV_TOOLS
// Diagnostics (recordRigidDraws): where the main camera's rigid model draws put their models, while asked.
std::atomic<bool> recordingRigid{};
std::mutex rigidMutex;
std::vector<RigidDraw> rigidDraws;
#endif

UINT byteWidth(ID3D11Buffer* buffer)
{
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);

    return desc.ByteWidth;
}

// Notes a skinned mesh's draw: its model's place in the world, the view's inverse (a rotation and a
// translation) times viewWorld's translation, as world is not kept current; the draw's index range; the
// bones it drew with.
void noteBones(ID3D11DeviceContext* context, const SceneConstants& scene, const ObjectConstants& object, const ui_redirect::DrawCall& call)
{
    ComPtr<ID3D11Buffer> buffer;
    context->VSGetConstantBuffers(BonesConstants::Slot, 1, buffer.GetAddressOf());
    if (!buffer || byteWidth(buffer.Get()) != sizeof(BonesConstants)) return;

    const auto* bones = constant_capture::writtenTo<BonesConstants>(buffer.Get());
    if (!bones) return;

    DrawnBones drawn{.firstIndex = call.first, .indexCount = call.count};
    const auto& view = scene.view;
    const float offset[3]{object.viewWorld[0][3] - view[0][3], object.viewWorld[1][3] - view[1][3], object.viewWorld[2][3] - view[2][3]};
    for (int i = 0; i < 3; ++i)
    {
        drawn.position[i] = view[0][i] * offset[0] + view[1][i] * offset[1] + view[2][i] * offset[2];
        drawn.eye[i] = scene.eye[i];
    }

    static_assert(sizeof(drawn.bones) == sizeof(BonesConstants::bones));
    std::memcpy(drawn.bones.data(), bones->bones, sizeof(drawn.bones));
    std::lock_guard lock(bonesMutex);
    if (drawnBones.size() < MaxDrawnBones) drawnBones.push_back(drawn);
}
}

void detach() noexcept
{
    active = false;
    constant_capture::keepAll(false);
    pending.clear();
    {
        std::lock_guard lock(executedMutex);
        executed.reset();
    }
}

void setActive(bool on) noexcept
{
    if (on == active.load()) return;

    std::lock_guard lock(executedMutex);
    active = on;
    constant_capture::keepAll(on);
    pending.clear();
    executed.reset();
}

void vertexShaderBound(ID3D11DeviceContext* context, VertexShader kind) noexcept
{
    boundContext = context;
    boundKind = kind;
}

void drawIndexed(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    const bool isModelOrTerrain = boundKind == VertexShader::Model || boundKind == VertexShader::Terrain;
    if (!active.load(std::memory_order_relaxed) || boundContext != context || !isModelOrTerrain || !call.count) return;

    const HookTimer timer;
    if constexpr (DiagnosticLog) ++workDraws;
    try
    {
        std::array<ComPtr<ID3D11Buffer>, 4> constants; // b2 to b5
        ID3D11Buffer* rawConstants[4]{};
        context->VSGetConstantBuffers(SceneConstants::Slot, 4, rawConstants);
        for (int i = 0; i < 4; ++i) constants[i].Attach(rawConstants[i]);

        const auto* scene = constant_capture::mainCameraWrite(constants[0].Get());
        if (!scene) return;

        pending.note(reinterpret_cast<std::uint64_t>(context), [&](Notes& notes) {
            if (!notes.camera) notes.camera = *scene;
        });

        const auto* object = constant_capture::writtenTo<ObjectConstants>(constants[3].Get());
        if (!object) return;

        // The terrain's inputs, with the object and material constants the game wrote for the draw.
        if (boundKind == VertexShader::Terrain)
        {
            const auto* material = constant_capture::writtenTo<TerrainMaterialConstants>(constants[2].Get());
            if (material) engine_scene::noteTerrain(context, *object, *material);
        }

        // A skinned mesh (useWeightCount set): its bones as drawn, for the scene's characters.
        if (boundKind == VertexShader::Model && object->useWeightCount[0] != 0) noteBones(context, *scene, *object, call);
#if PHANTOM_FX_DEV_TOOLS
        if (boundKind == VertexShader::Model && object->useWeightCount[0] == 0 && recordingRigid.load(std::memory_order_relaxed))
        {
            RigidDraw drawn{.firstIndex = call.first, .indexCount = call.count};
            const auto& view = scene->view;
            const float offset[3]{object->viewWorld[0][3] - view[0][3], object->viewWorld[1][3] - view[1][3], object->viewWorld[2][3] - view[2][3]};
            for (int i = 0; i < 3; ++i) drawn.position[i] = view[0][i] * offset[0] + view[1][i] * offset[1] + view[2][i] * offset[2];
            std::lock_guard lock(rigidMutex);
            if (rigidDraws.size() < (1u << 16)) rigidDraws.push_back(drawn);
        }
#endif
    }
    catch (...)
    {
    }
}

void recorded(std::uint64_t into, std::uint64_t from) noexcept
{
    pending.recorded(into, from);
}

void forget(std::uint64_t contextOrList) noexcept
{
    pending.forget(contextOrList);
}

void executing(std::uint64_t list) noexcept
{
    const auto noted = pending.take(list);
    if (!noted || !noted->camera) return;

    std::lock_guard lock(executedMutex);
    executed = noted->camera;
}

std::size_t present() noexcept
{
    return pending.present();
}

#if PHANTOM_FX_DEV_TOOLS
void recordRigidDraws(bool on) noexcept
{
    std::lock_guard lock(rigidMutex);
    rigidDraws.clear();
    recordingRigid = on;
}

std::vector<RigidDraw> takeRigidDraws() noexcept
{
    std::lock_guard lock(rigidMutex);
    recordingRigid = false;
    return std::exchange(rigidDraws, {});
}
#endif

std::vector<DrawnBones> takeDrawnBones(const SceneConstants& camera) noexcept
{
    std::vector<DrawnBones> taken;
    try
    {
        std::lock_guard lock(bonesMutex);
        for (const auto& drawn : drawnBones)
        {
            bool sameEye = true;
            for (int i = 0; i < 3; ++i) sameEye = sameEye && std::abs(drawn.eye[i] - camera.eye[i]) <= 1e-3f;
            if (sameEye) taken.push_back(drawn);
        }

        drawnBones.clear();
    }
    catch (...)
    {
    }

    return taken;
}

std::optional<SceneConstants> takeCamera() noexcept
{
    std::lock_guard lock(executedMutex);
    return std::exchange(executed, std::nullopt);
}

Work takeWork() noexcept
{
    return {clock::millisecondsOf(hookTicks.exchange(0)), workDraws.exchange(0)};
}
}
