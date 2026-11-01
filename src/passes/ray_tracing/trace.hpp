#pragma once

#include "passes/ray_tracing/gi_d3d12.hpp"
#include "passes/ray_tracing/gi_renderer.hpp"
#include "passes/ray_tracing/kick_traces.hpp"
#include "passes/ray_tracing/scene_contents.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"
#include "passes/ray_tracing/shared_textures.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// A trace: one job the rays do for a frame, owning what it needs on both Direct3D sides (docs/DEVELOPMENT.md, "What
// a kick traces"). Which traces a kick runs is decided in kick_traces.hpp; the frame walks them through these steps.
namespace phantom_fx::trace
{
// What a trace has at the kick, on Direct3D 11.
struct Kick
{
    ID3D11Device* device{};
    UINT width{}, height{};
    const kick_traces::KickTraces& traces;
};

// What a trace has of the frame, on Direct3D 12.
struct TraceFrame
{
    const gi_renderer::Frame& frame;
    ID3D12Device* device{};
    const std::function<void()>& waitForAll; // For memory the frames in flight may still use
    const scene_contents::SceneContents& contents;
    const std::vector<scene_contents::InstancedModel>& instanced; // By instance; the slot's last frame's until the scene's contents update
    std::uint32_t slot{};
};

// What a trace binds its frame's work to.
struct TraceBindings
{
    gi::GIFrameConstants& constants;
    gi_d3d12::Descriptors& descriptors;
    shared_textures::SharedBindings& shared;
};

class Trace
{
public:
    virtual ~Trace() = default;

    virtual bool runsIn(const kick_traces::KickTraces& traces) const = 0;
    // Whether its frame needs the scene's instances listed, as a hit names them by index.
    virtual bool listsInstances(const gi_renderer::Frame& frame) const = 0;

    // Every kick, whether it runs or not, before Direct3D 12 may read the kick's inputs. Throws on Direct3D errors.
    virtual void gather(const Kick& kick) = 0;
    // Every frame, whether it runs or not: its memory at the frame's size, and what its last frame in this slot left to
    // read back. Throws on Direct3D errors.
    virtual void prepare(const TraceFrame& frame) = 0;
    // Where it runs: its constants, its shared textures and its own views.
    virtual void bind(const TraceFrame& frame, TraceBindings& bindings) = 0;
    virtual void record(const TraceFrame& frame, gi_d3d12::Recorder& recorder) = 0;
    // The rays stop: its Direct3D 12 memory back. Its results stay for their readers.
    virtual void release() noexcept = 0;
    // Everything back; only when no Direct3D work can run.
    virtual void detach() noexcept = 0;
};

// The kick's traces, in the order the frame runs them.
std::span<Trace* const> inOrder() noexcept;
}
