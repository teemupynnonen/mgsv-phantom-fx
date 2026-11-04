#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

// Nsight Graphics' GPU Trace of the ray tracing queue, started and stopped from the add-on (docs/DEVELOPMENT.md,
// "GPU Trace"). GPU Trace marks frames by Direct3D 12 presents, and the add-on's Direct3D 12 work never presents, so
// its own trigger cannot find a frame; the automation's `gputrace <frames>` asks for one through Nsight's SDK instead.
// Needs the game launched by ngfx with --start-with-ngfx-sdk --stop-with-ngfx-sdk and PHANTOM_FX_PROFILING set.
namespace phantom_fx::gpu_trace
{
// Whether this build has Nsight's SDK (dev.cmake finds it in the Nsight Graphics install).
bool isAvailable() noexcept;
// Traces the ray tracing queue's next `submits` submissions (frames).
void request(std::uint32_t submits) noexcept;
// The line that says how the last request went.
std::string lastResult();

// The ray tracing thread, around each submission of its queue.
void beforeSubmit(ID3D12CommandQueue* queue) noexcept;
void afterSubmit(ID3D12CommandQueue* queue) noexcept;
}
