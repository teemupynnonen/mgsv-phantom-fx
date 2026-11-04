#include "dev/gpu_trace.hpp"

#include "log.hpp"

#include <atomic>
#include <mutex>

#if PHANTOM_FX_NSIGHT_SDK
#include <NGFX_GPUTrace_D3D12.h>
#endif

namespace phantom_fx::gpu_trace
{
namespace
{
std::atomic<std::uint32_t> requested{};

std::mutex resultMutex;
std::string result = "no trace asked for";

// The ray tracing thread's alone.
std::uint32_t remaining{};
bool isActivated{}, isTracing{};

void report(const std::string& line)
{
    log("GPU Trace: " + line);
    const std::lock_guard lock(resultMutex);
    result = line;
}
}

bool isAvailable() noexcept
{
    return PHANTOM_FX_NSIGHT_SDK;
}

void request(std::uint32_t submits) noexcept
{
    requested.store(submits ? submits : 1);
}

std::string lastResult()
{
    const std::lock_guard lock(resultMutex);
    return result;
}

void beforeSubmit([[maybe_unused]] ID3D12CommandQueue* queue) noexcept
{
#if PHANTOM_FX_NSIGHT_SDK
    if (isTracing) return;

    const auto submits = requested.exchange(0);
    if (!submits) return;

    if (!isActivated)
    {
        NGFX_GPUTrace_InitializeActivity_D3D12_Params initialize{NGFX_GPUTrace_InitializeActivity_D3D12_Params_VER};
        if (const auto failure = NGFX_GPUTrace_InitializeActivity_D3D12(&initialize))
            return report("Nsight did not start (" + std::to_string(failure) + "): was the game launched by ngfx with --start-with-ngfx-sdk?");

        NGFX_GPUTrace_ActivateTrace_D3D12_Params activate{NGFX_GPUTrace_ActivateTrace_D3D12_Params_VER, queue};
        if (const auto failure = NGFX_GPUTrace_ActivateTrace_D3D12(&activate)) return report("activating failed (" + std::to_string(failure) + ")");

        isActivated = true;
    }

    NGFX_GPUTrace_StartTrace_D3D12_Params start{NGFX_GPUTrace_StartTrace_D3D12_Params_VER};
    if (const auto failure = NGFX_GPUTrace_StartTrace_D3D12(&start)) return report("starting failed (" + std::to_string(failure) + ")");

    remaining = submits;
    isTracing = true;
    report("tracing the next " + std::to_string(submits) + " submissions");
#endif
}

void afterSubmit([[maybe_unused]] ID3D12CommandQueue* queue) noexcept
{
#if PHANTOM_FX_NSIGHT_SDK
    if (!isTracing || --remaining) return;

    // Done once the queue has finished what was submitted while tracing.
    NGFX_GPUTrace_StopTrace_D3D12_Params stop{NGFX_GPUTrace_StopTrace_D3D12_Params_VER, NGFX_GPUTrace_StopTraceFlag_None, queue, nullptr, 0};
    isTracing = false;
    if (const auto failure = NGFX_GPUTrace_StopTrace_D3D12(&stop)) return report("stopping failed (" + std::to_string(failure) + ")");

    report("stopped; ngfx writes the trace");
#endif
}
}
