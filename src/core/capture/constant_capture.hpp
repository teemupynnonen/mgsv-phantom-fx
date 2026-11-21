#pragma once

#include "game_constants.hpp"

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <optional>

// The constant capture: what each of the game's threads last wrote to each of its constant buffers,
// copied as the game unmaps them, for the add-on to read by name (game_constants.hpp) at the draws that
// use them. A thread reads only its own writes, as the draws its context records read what it wrote
// before them, and only a write of the size it reads.
namespace phantom_fx::constant_capture
{
// While on, the scene (b2) and light (b3) constants are kept, for the contact shadows.
void keepLightPasses(bool keep) noexcept;
// While on, every buffer the ray tracing reads is kept too, and each scene constants write notes whether
// it is the main camera's.
void keepAll(bool keep) noexcept;

// ---- Recording, on the game's render and worker threads ----
// The game's Map and Unmap of any buffer, after the upscaler's: a kept constant buffer is copied at its
// unmap, so the scene constants are copied with the upscaler's jitter in them.
void bufferMapped(ID3D11Resource* buffer, void* data) noexcept;
void bufferUnmapped(ID3D11Resource* buffer) noexcept;

namespace detail
{
// This thread's last write to `buffer` when it was `size` bytes; null when none, or one of another size,
// which the diagnostic log counts.
const void* writtenAs(ID3D11Buffer* buffer, std::size_t size) noexcept;
}

// What this thread last wrote to `buffer`, as C; null when it wrote nothing of C's size. Valid until this
// thread writes to `buffer` again: for the reads on every draw, which copy nothing.
template<class C> const C* writtenTo(ID3D11Buffer* buffer) noexcept
{
    return static_cast<const C*>(detail::writtenAs(buffer, sizeof(C)));
}

// The same, copied.
template<class C> std::optional<C> written(ID3D11Buffer* buffer) noexcept
{
    const auto* found = writtenTo<C>(buffer);
    if (!found) return std::nullopt;

    return *found;
}

// What the pixel shader bound on `context` reads at C's slot.
template<class C> std::optional<C> pixelShaderReads(ID3D11DeviceContext* context) noexcept
{
    ID3D11Buffer* buffer{};
    context->PSGetConstantBuffers(C::Slot, 1, &buffer);
    auto read = written<C>(buffer);
    if (buffer) buffer->Release();

    return read;
}

// This thread's last write to `buffer` when it was the main camera's scene constants (camera::isMainCamera)
// while all were kept; null otherwise. Valid as writtenTo's.
const game_constants::SceneConstants* mainCameraWrite(ID3D11Buffer* buffer) noexcept;

// Diagnostics, with the diagnostic log: the unmap hook's time on the game's threads, all threads
// together, and the reads refused for a write of another size, since the last call.
struct Work
{
    double unmapMilliseconds{};
    std::uint64_t refusedReads{};
};
Work takeWork() noexcept;
}
