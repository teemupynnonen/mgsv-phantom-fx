#pragma once

#include "d3d12_helpers.hpp"

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Direct3D 12 work at a point of the game's Direct3D 11 immediate context, for what runs on Direct3D 12 only: FSR
// (fsr_upscaler.hpp) and Ray Reconstruction (ray_reconstruction.hpp). A native Direct3D 12 device on the game's GPU
// works on textures Direct3D 11 shares, and the two take turns through a shared fence.
namespace phantom_fx::d3d12_host
{
// One evaluation's command list and shared textures (Host::evaluate).
class Evaluation
{
public:
    ID3D12GraphicsCommandList* list() const { return commands; }

    // `texture` on Direct3D 12, through `opened`, which keeps it open from frame to frame, in the state it is read or
    // written in until the evaluation ends.
    ID3D12Resource* read(OpenedTexture& opened, ID3D11Texture2D* texture, const char* opening);
    ID3D12Resource* write(OpenedTexture& opened, ID3D11Texture2D* texture, const char* opening);

private:
    friend class Host;

    Evaluation(ID3D12Device* device, ID3D12GraphicsCommandList* list) : device(device), commands(list) {}

    ID3D12Resource* use(OpenedTexture& opened, ID3D11Texture2D* texture, const char* opening, D3D12_RESOURCE_STATES state);
    void returnTextures();

    ID3D12Device* device;
    ID3D12GraphicsCommandList* commands;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
};

class Host
{
public:
    // On the thread executing Direct3D work, as everything here. `name` says whose work it is in its errors. Throws
    // on Direct3D errors.
    Host(ID3D11Device* device, std::string name);

    ID3D12Device* device() const { return device12.Get(); }

    // Runs what `record` records at this point of the immediate context: Direct3D 12 waits for the work recorded so far,
    // and the work recorded after waits for it. Throws when `record` or Direct3D fails, and the next one can still run.
    void evaluate(const std::function<void(Evaluation&)>& record);
    // Before freeing what the last evaluation used. Throws when it does not finish.
    void waitForLast();

private:
    static constexpr std::size_t ListsInFlight = 3;

    void waitFor(UINT64 value);
    // Fails naming whose work it was.
    void checkStep(HRESULT result, std::string_view before, std::string_view after = {}) const;

    std::string name;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence11;

    // Native: ReShade wraps the device it creates, and other add-ons would see this one as a game device. The proxy
    // stays alive while its device is used.
    Microsoft::WRL::ComPtr<ID3D12Device> wrapper, device12;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12;
    std::unique_ptr<void, HandleCloser> event;

    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, ListsInFlight> allocators;
    std::array<UINT64, ListsInFlight> submitted{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 fenceValue{}, evaluations{};
};
}
