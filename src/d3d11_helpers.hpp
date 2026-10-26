#pragma once

#include "hresult.hpp"
#include "log.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// Direct3D 11 helpers shared by the add-on's effects: their shaders, constants and working textures.
namespace phantom_fx
{
// Flags for a texture Direct3D 12 opens through an NT handle.
constexpr UINT SharedWithD3D12 = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

// A 2D texture compute shaders write and read, with its views.
struct ComputeTexture
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> access;
    UINT width{}, height{};
};

// Compiles `source`'s main for `profile`; throws with the compiler's errors.
inline Microsoft::WRL::ComPtr<ID3DBlob> compile(const char* source, const char* name, const char* profile, const D3D_SHADER_MACRO* defines = nullptr)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source, std::strlen(source), name, defines, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error(std::string("Shader ") + name + " did not compile: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                      : hex(static_cast<std::uint32_t>(result))));

    return code;
}

inline Microsoft::WRL::ComPtr<ID3D11ComputeShader> computeShader(ID3D11Device* device, const char* source, const char* name,
    const D3D_SHADER_MACRO* defines = nullptr)
{
    const auto code = compile(source, name, "cs_5_0", defines);
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf()), name);

    return shader;
}

inline Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader(ID3D11Device* device, const char* source, const char* name,
    const D3D_SHADER_MACRO* defines = nullptr)
{
    const auto code = compile(source, name, "ps_5_0", defines);
    Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
    check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf()), name);

    return shader;
}

// A constant buffer the CPU rewrites before each use, through write().
inline Microsoft::WRL::ComPtr<ID3D11Buffer> makeConstantBuffer(ID3D11Device* device, UINT size, const char* what)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc, nullptr, buffer.GetAddressOf()), what);

    return buffer;
}

// Replaces a makeConstantBuffer()'s contents. Returns Map's result: callers choose whether a failure throws.
template<class Data> [[nodiscard]] HRESULT write(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const Data& data)
{
    static_assert(std::is_trivially_copyable_v<Data>);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto result = context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(result)) return result;

    std::memcpy(mapped.pData, &data, sizeof(data));
    context->Unmap(buffer, 0);

    return result;
}

// `miscFlags` is SharedWithD3D12 for a texture Direct3D 12 reads or writes too.
inline ComputeTexture makeComputeTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, const char* what, UINT miscFlags = 0)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = miscFlags;
    ComputeTexture made{.width = width, .height = height};
    check(device->CreateTexture2D(&desc, nullptr, made.texture.GetAddressOf()), what);
    check(device->CreateShaderResourceView(made.texture.Get(), nullptr, made.view.GetAddressOf()), what);
    check(device->CreateUnorderedAccessView(made.texture.Get(), nullptr, made.access.GetAddressOf()), what);

    return made;
}

inline Microsoft::WRL::ComPtr<ID3D11Resource> resourceOf(ID3D11View* view)
{
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    if (view) view->GetResource(resource.GetAddressOf());

    return resource;
}

// Zeros when `resource` is not a 2D texture.
inline D3D11_TEXTURE2D_DESC textureDesc(ID3D11Resource* resource)
{
    D3D11_TEXTURE2D_DESC desc{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) texture->GetDesc(&desc);

    return desc;
}

inline D3D11_TEXTURE2D_DESC textureDesc(ID3D11View* view)
{
    return textureDesc(resourceOf(view).Get());
}

// The size of the 2D texture behind `view`; zero when there is none.
inline std::pair<UINT, UINT> sizeOf(ID3D11View* view)
{
    const auto desc = textureDesc(view);
    return {desc.Width, desc.Height};
}

// Thread groups for the 8 x 8 kernels to cover `texels` along an axis.
inline UINT groups(UINT texels)
{
    return (texels + 7) / 8;
}

// How many of the compute stage's first slots of each kind a SavedComputeState keeps.
struct ComputeSlots
{
    UINT buffers{}, views{}, accesses{}, samplers{};
};

// The game's compute shader and its first slots, put back when this goes out of scope.
class SavedComputeState
{
public:
    SavedComputeState(ID3D11DeviceContext* context, ComputeSlots slots) : context(context), slots(slots)
    {
        context->CSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        context->CSGetConstantBuffers(0, slots.buffers, buffers);
        context->CSGetShaderResources(0, slots.views, views);
        context->CSGetUnorderedAccessViews(0, slots.accesses, accesses);
        context->CSGetSamplers(0, slots.samplers, samplers);
    }

    ~SavedComputeState()
    {
        // The add-on's outputs go first, so none is bound for writing while the game's views of the
        // same texture are bound for reading.
        ID3D11UnorderedAccessView* const none[D3D11_1_UAV_SLOT_COUNT]{};
        context->CSSetUnorderedAccessViews(0, slots.accesses, none, nullptr);
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, slots.buffers, buffers);
        context->CSSetShaderResources(0, slots.views, views);
        context->CSSetUnorderedAccessViews(0, slots.accesses, accesses, nullptr);
        context->CSSetSamplers(0, slots.samplers, samplers);
        release(buffers, slots.buffers);
        release(views, slots.views);
        release(accesses, slots.accesses);
        release(samplers, slots.samplers);
    }

    SavedComputeState(const SavedComputeState&) = delete;
    SavedComputeState& operator=(const SavedComputeState&) = delete;

private:
    template<class Object> static void release(Object* const* objects, UINT count)
    {
        for (UINT i = 0; i < count; ++i)
            if (objects[i]) objects[i]->Release();
    }

    ID3D11DeviceContext* context;
    ComputeSlots slots;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    // Up to Direct3D 11.1's slot counts.
    ID3D11Buffer* buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    ID3D11UnorderedAccessView* accesses[D3D11_1_UAV_SLOT_COUNT]{};
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
};

// One viewport and scissor rectangle over `width` x `height` from 0,0; the game's are put back when
// this goes out of scope.
class WholeTargetViewport
{
public:
    WholeTargetViewport(ID3D11DeviceContext* context, UINT width, UINT height) : context(context)
    {
        context->RSGetViewports(&viewportCount, viewports);
        context->RSGetScissorRects(&rectCount, rects);

        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        const D3D11_RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        context->RSSetViewports(1, &viewport);
        context->RSSetScissorRects(1, &rect);
    }

    ~WholeTargetViewport()
    {
        context->RSSetViewports(viewportCount, viewports);
        context->RSSetScissorRects(rectCount, rects);
    }

    WholeTargetViewport(const WholeTargetViewport&) = delete;
    WholeTargetViewport& operator=(const WholeTargetViewport&) = delete;

private:
    ID3D11DeviceContext* context;

    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    // The array sizes; the getters write back how many are bound.
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT rectCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
};
}
