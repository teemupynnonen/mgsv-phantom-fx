// The constant capture; constant_capture.hpp describes it.
#include "constant_capture.hpp"

#include "camera.hpp"
#include "clock.hpp"
#include "log.hpp"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>

namespace phantom_fx::constant_capture
{
namespace
{
using namespace game_constants;

// The bones are the largest buffer kept.
constexpr UINT LargestConstants = sizeof(BonesConstants) / 4;

std::atomic<bool> lightPassesKept{}, allKept{};

// What the thread last wrote to each kept constant buffer, and its size in bytes; for scene constants,
// whether they are the main camera's, worked out once a write while all are kept. Kept by the buffer's
// address: a buffer the game makes at a released one's address reads as the old one's last write of the
// same size until it is written.
struct Written
{
    std::array<float, LargestConstants> values{};
    UINT size{};
    bool mainCamera{};
};

thread_local ID3D11Resource* mappedBuffer{};
thread_local void* mappedData{};
thread_local UINT mappedSize{};
thread_local std::unordered_map<ID3D11Resource*, Written> contents;

// Diagnostics: the unmap hook's time in performance counter ticks, and the refused reads, since takeWork.
std::atomic<std::int64_t> unmapTicks{};
std::atomic<std::uint64_t> refusedReads{};

// A write as C, whatever its size.
template<class C> const C& as(const Written& written)
{
    return *reinterpret_cast<const C*>(written.values.data());
}

// The contact shadows read the light passes' scene and light constants; the ray tracing also the
// materials, objects, bones and probes.
bool isLightPassSize(UINT size)
{
    return size == sizeof(SceneConstants) || size == sizeof(SunLightConstants) || size == sizeof(LampLightConstants);
}

bool isRayTracingSize(UINT size)
{
    return size == sizeof(LampMaterialConstants) || size == sizeof(TerrainMaterialConstants) || size == sizeof(ObjectConstants)
        || size == sizeof(BonesConstants) || size == sizeof(ProbeConstants);
}

// The first refused read since the last takeWork is logged with its sizes, the rest counted.
void noteRefused(std::size_t size, UINT writtenSize) noexcept
{
    if (refusedReads.fetch_add(1, std::memory_order_relaxed) != 0) return;

    try
    {
        log("Constant capture: a read of " + std::to_string(size) + " bytes refused this thread's last write of " + std::to_string(writtenSize)
            + " bytes to that buffer.");
    }
    catch (...)
    {
    }
}

// This thread's last write to `buffer` when it was `size` bytes.
const Written* writtenOfSize(ID3D11Buffer* buffer, std::size_t size) noexcept
{
    if (!buffer) return nullptr;

    const auto found = contents.find(buffer);
    if (found == contents.end()) return nullptr;

    if (found->second.size != size)
    {
        if constexpr (DiagnosticLog) noteRefused(size, found->second.size);
        return nullptr;
    }

    return &found->second;
}
}

void keepLightPasses(bool keep) noexcept
{
    lightPassesKept = keep;
}

void keepAll(bool keep) noexcept
{
    allKept = keep;
}

void bufferMapped(ID3D11Resource* buffer, void* data) noexcept
{
    const bool all = allKept.load(std::memory_order_relaxed);
    if ((!all && !lightPassesKept.load(std::memory_order_relaxed)) || !data) return;

    D3D11_RESOURCE_DIMENSION dimension{};
    buffer->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return;

    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(buffer)->GetDesc(&desc);
    const UINT size = desc.ByteWidth;
    if (!(isLightPassSize(size) || (all && isRayTracingSize(size))) || !(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;

    mappedBuffer = buffer;
    mappedData = data;
    mappedSize = size;
}

void bufferUnmapped(ID3D11Resource* buffer) noexcept
{
    if (!mappedBuffer || buffer != mappedBuffer) return;

    // The time is added up only with the diagnostic log, as the game's threads would share the counter
    // on every unmap.
    LONGLONG started{};
    if constexpr (DiagnosticLog) started = clock::ticksNow();

    try
    {
        auto& written = contents[buffer];
        std::memcpy(written.values.data(), mappedData, mappedSize);
        written.size = mappedSize;
        if (mappedSize == sizeof(SceneConstants))
            written.mainCamera = allKept.load(std::memory_order_relaxed) && camera::isMainCamera(as<SceneConstants>(written));
    }
    catch (...)
    {
    }

    mappedBuffer = nullptr;
    mappedData = nullptr;
    if constexpr (DiagnosticLog) unmapTicks += clock::ticksNow() - started;
}

namespace detail
{
const void* writtenAs(ID3D11Buffer* buffer, std::size_t size) noexcept
{
    const auto* written = writtenOfSize(buffer, size);
    return written ? written->values.data() : nullptr;
}
}

const SceneConstants* mainCameraWrite(ID3D11Buffer* buffer) noexcept
{
    const auto* written = writtenOfSize(buffer, sizeof(SceneConstants));
    return written && written->mainCamera ? &as<SceneConstants>(*written) : nullptr;
}

Work takeWork() noexcept
{
    return {clock::millisecondsOf(unmapTicks.exchange(0)), refusedReads.exchange(0)};
}
}
