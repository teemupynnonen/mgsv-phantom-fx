#include "passes/ray_tracing/shared_textures.hpp"

namespace phantom_fx::shared_textures
{
ID3D11Texture2D* SharedOutput::ensure(ID3D11Device* device, UINT width, UINT height)
{
    std::lock_guard lock(mutex);
    if (made.width != width || made.height != height)
    {
        made = makeComputeTexture(device, width, height, format, creating, SharedWithD3D12);
        isWritten = false;
    }

    return made.texture.Get();
}

void SharedOutput::markWritten(const ID3D11Texture2D* written) noexcept
{
    std::lock_guard lock(mutex);
    isWritten = made.texture.Get() == written;
}

void SharedOutput::clearWritten() noexcept
{
    std::lock_guard lock(mutex);
    isWritten = false;
}

ID3D11ShaderResourceView* SharedOutput::writtenView(UINT width, UINT height) const noexcept
{
    std::lock_guard lock(mutex);

    return isWritten && made.width == width && made.height == height ? made.view.Get() : nullptr;
}

void SharedOutput::reset() noexcept
{
    std::lock_guard lock(mutex);
    made = {};
    isWritten = false;
}

ID3D12Resource* SharedBindings::share(const SharedRow& row, ID3D11Texture2D* texture, OpenedTexture& opened, SharedOutput* output)
{
    check(opened.open(device, texture), row.opening);
    auto* resource = opened.resource.Get();
    const auto* written = std::get_if<binding::OutputTextureSlot>(&row.slot);
    if (written) descriptors.outputTexture(*written, resource, row.format);
    else descriptors.texture(std::get<binding::TextureSlot>(row.slot), resource, row.format);

    uses.push_back({resource, written ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, texture, output});

    return resource;
}

void SharedBindings::addTransitions(std::vector<D3D12_RESOURCE_BARRIER>& barriers, bool isToCommon) const
{
    for (const auto& use : uses)
        barriers.push_back(isToCommon ? transition(use.resource, use.state, D3D12_RESOURCE_STATE_COMMON)
                                      : transition(use.resource, D3D12_RESOURCE_STATE_COMMON, use.state));
}

void SharedBindings::markWritten() const noexcept
{
    for (const auto& use : uses)
        if (use.output) use.output->markWritten(use.texture);
}
}
