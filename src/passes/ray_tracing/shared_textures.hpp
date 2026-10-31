#pragma once

#include "d3d11_helpers.hpp"
#include "d3d12_helpers.hpp"
#include "passes/ray_tracing/gi_d3d12.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <vector>

// Shared textures, which the game's Direct3D 11 side and the rays' Direct3D 12 side both use (docs/DEVELOPMENT.md,
// "What a kick traces"). A trace keeps the Direct3D 11 half of what it writes in a SharedOutput, and declares each
// texture it reads or writes in a SharedRow.
namespace phantom_fx::shared_textures
{
// The Direct3D 11 half of what the rays write. The kick and the threads recording the next frame ask for it at
// the same time, so it keeps its own lock.
class SharedOutput
{
public:
    SharedOutput(DXGI_FORMAT format, const char* creating) : format(format), creating(creating) {}

    // Makes it at this size unless it has it, which leaves it unwritten; its texture. Throws on Direct3D errors.
    ID3D11Texture2D* ensure(ID3D11Device* device, UINT width, UINT height);
    // A frame traced into `written`; if a new size replaced it since, what it holds now is not written.
    void markWritten(const ID3D11Texture2D* written) noexcept;
    void clearWritten() noexcept;
    // Null until written, or at another size.
    ID3D11ShaderResourceView* writtenView(UINT width, UINT height) const noexcept;
    void reset() noexcept;

private:
    mutable std::mutex mutex;
    DXGI_FORMAT format;
    const char* creating;
    ComputeTexture made;
    bool isWritten{};
};

enum class Access : std::uint8_t { Read, Write };

// One shared texture a trace uses: how, and at which slot of the frame's tables, the shaders' or the outputs'
// by its access.
struct SharedRow
{
    DXGI_FORMAT format{};
    Access access{};
    UINT slot{};
    const char* opening{};
};

// The frame's shared textures as its traces declare them.
class SharedBindings
{
public:
    SharedBindings(ID3D12Device* device, gi_d3d12::Descriptors& descriptors) : device(device), descriptors(descriptors) {}

    // Opens `texture` in `opened`, which keeps it open from frame to frame, and puts it in the frame's tables. An
    // `output` the frame writes is marked written once the frame is submitted.
    ID3D12Resource* share(const SharedRow& row, ID3D11Texture2D* texture, OpenedTexture& opened, SharedOutput* output = nullptr);

    // Out of the common state for the frame's passes, or back to it.
    void addTransitions(std::vector<D3D12_RESOURCE_BARRIER>& barriers, bool isToCommon) const;
    void markWritten() const noexcept;

private:
    struct Use
    {
        ID3D12Resource* resource{};
        D3D12_RESOURCE_STATES state{};
        ID3D11Texture2D* texture{};
        SharedOutput* output{};
    };

    ID3D12Device* device;
    gi_d3d12::Descriptors& descriptors;
    std::vector<Use> uses;
};
}
