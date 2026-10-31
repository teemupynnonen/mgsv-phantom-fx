#include "features/upscaling/d3d12_host.hpp"

#include <Windows.h>
#include <stdexcept>
#include <utility>

namespace phantom_fx::d3d12_host
{
ID3D12Resource* Evaluation::read(OpenedTexture& opened, ID3D11Texture2D* texture, const char* opening)
{
    return use(opened, texture, opening, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

ID3D12Resource* Evaluation::write(OpenedTexture& opened, ID3D11Texture2D* texture, const char* opening)
{
    return use(opened, texture, opening, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

ID3D12Resource* Evaluation::use(OpenedTexture& opened, ID3D11Texture2D* texture, const char* opening, D3D12_RESOURCE_STATES state)
{
    check(opened.open(device, texture), opening);
    auto* resource = opened.resource.Get();
    const auto barrier = transition(resource, D3D12_RESOURCE_STATE_COMMON, state);
    commands->ResourceBarrier(1, &barrier);
    barriers.push_back(barrier);

    return resource;
}

void Evaluation::returnTextures()
{
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    if (!barriers.empty()) commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}

Host::Host(ID3D11Device* device, std::string name) : name(std::move(name))
{
    Microsoft::WRL::ComPtr<ID3D11Device5> device11;
    check(device->QueryInterface(IID_PPV_ARGS(&device11)), "Finding Direct3D 11.4");
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device11->GetImmediateContext(context.GetAddressOf());
    check(context.As(&context11), "Finding the Direct3D 11.4 context");
    check(device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)), "Creating the shared fence");

    const auto d3d12 = createDevice12(device11.Get());
    wrapper = d3d12.wrapper;
    device12 = d3d12.native;

    const D3D12_COMMAND_QUEUE_DESC queueDesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    check(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Creating the Direct3D 12 queue");
    for (auto& allocator : allocators)
        check(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Creating a command allocator");

    check(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list)), "Creating the command list");
    check(list->Close(), "Closing the command list");
    HANDLE fence{};
    check(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence), "Sharing the fence");
    check(openSharedHandle(device12.Get(), fence, fence12), "Opening the shared fence");
    event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!event) throw std::runtime_error("Creating the fence event failed.");
}

void Host::evaluate(const std::function<void(Evaluation&)>& record)
{
    // Direct3D 12 starts once Direct3D 11 has drawn the inputs.
    const auto drawn = ++fenceValue;
    checkStep(context11->Signal(fence11.Get(), drawn), "Signalling the ", " inputs");
    context11->Flush();

    const auto slot = evaluations++ % ListsInFlight;
    waitFor(submitted[slot]);
    check(allocators[slot]->Reset(), "Resetting a command allocator");
    check(list->Reset(allocators[slot].Get(), nullptr), "Resetting the command list");
    try
    {
        Evaluation evaluation(device12.Get(), list.Get());
        record(evaluation);
        evaluation.returnTextures();
    }
    catch (...)
    {
        // The next one resets the list, which it can only once closed.
        list->Close();
        throw;
    }

    checkStep(list->Close(), "Recording ");
    checkStep(queue->Wait(fence12.Get(), drawn), "Waiting for the ", " inputs");
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const auto done = ++fenceValue;
    checkStep(queue->Signal(fence12.Get(), done), "Signalling the ", " output");
    submitted[slot] = done;

    // What comes after reads the output.
    checkStep(context11->Wait(fence11.Get(), done), "Waiting for ");
}

void Host::waitForLast()
{
    waitFor(fenceValue);
}

void Host::waitFor(UINT64 value)
{
    if (fence12->GetCompletedValue() >= value) return;

    check(fence12->SetEventOnCompletion(value, event.get()), "Waiting for Direct3D 12");
    if (WaitForSingleObject(event.get(), FenceTimeout) != WAIT_OBJECT_0) throw std::runtime_error("Direct3D 12 did not finish " + name + " within 10 seconds.");
}

void Host::checkStep(HRESULT result, std::string_view before, std::string_view after) const
{
    if (FAILED(result)) check(result, (std::string(before) + name + std::string(after)).c_str());
}
}
