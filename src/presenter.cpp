// The game's stand-in swap chain; presenter.hpp describes the route.
#include "presenter.hpp"

#include "d3d12_helpers.hpp"
#include "fsr_frame_generation.hpp"
#include "game_code.hpp"
#include "log.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace phantom_fx::presenter
{
namespace
{
using Microsoft::WRL::ComPtr;

// IDXGIFactory::CreateSwapChain in the factory's virtual function table. The game makes its swap
// chain with it, not with CreateSwapChainForHwnd.
constexpr std::size_t CreateSwapChainSlot = 10;
// Command allocators in rotation; the swap chain lets at most three frames queue.
constexpr std::size_t FramesInFlight = 4;

using CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain);

std::filesystem::path addonDirectory;
Route presentRoute{};
std::atomic<bool> hooked{};
// DXGI's own CreateSwapChain, past the detour.
CreateSwapChain originalCreate{};
std::atomic<bool> presenting{};
// From the add-on's present event to the stand-in's Present, on the presenting thread.
std::optional<frame_generation::Inputs> submittedFrame;

std::mutex statusMutex;
std::string statusText; // Empty until install()

void setStatus(std::string text)
{
    log(text);
    std::lock_guard lock(statusMutex);
    statusText = std::move(text);
}

// A failed Direct3D call, with its result for the game when it happens inside a swap chain method.
struct Failure : std::runtime_error
{
    HRESULT result;

    Failure(const std::string& what, HRESULT result) : std::runtime_error(what), result(result) {}
};

void check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw Failure(what + std::string(" failed (") + hex(static_cast<std::uint32_t>(result)) + ").", result);
}

void logFailure(const std::exception& failure)
{
    static bool logged{};
    if (!std::exchange(logged, true)) log(failure.what());
}

// The native object behind a ReShade proxy, or the object itself when ReShade does not wrap it.
template<class Interface> ComPtr<Interface> unwrapped(IUnknown* object)
{
    ComPtr<IUnknown> native;
    if (FAILED(object->QueryInterface(UnwrappedObject, reinterpret_cast<void**>(native.GetAddressOf())))) native = object;

    ComPtr<Interface> result;
    native.As(&result);

    return result;
}

// Streamline's proxy of a native object, which it presents through.
template<class Interface> ComPtr<Interface> upgraded(IUnknown* native, const char* what)
{
    ComPtr<IUnknown> proxy;
    proxy.Attach(frame_generation::upgrade(native));
    ComPtr<Interface> result;
    if (!proxy || FAILED(proxy.As(&result))) throw std::runtime_error(std::string("Streamline did not take ") + what + ".");

    return result;
}

ComPtr<ID3D12Resource> openTexture(ID3D12Device* device, ID3D11Texture2D* texture, const char* what)
{
    ComPtr<ID3D12Resource> opened;
    check(openInD3D12(device, texture, opened), what);
    return opened;
}

// Direct3D 12's view of a texture the upscaler shares, opened again when the upscaler replaces it.
ID3D12Resource* openInput(ID3D12Device* device, OpenedTexture& input, ID3D11Texture2D* texture)
{
    check(input.open(device, texture), "Opening a frame generation input");
    return input.resource.Get();
}

// Flip model swap chains take no sRGB formats; the game's back buffer keeps its own, and the copy
// moves the same bits, so the real swap chain holds what the game's would.
DXGI_FORMAT presentable(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return format;
    }
}

// What the game holds in place of its swap chain. The game renders into `backBuffer`; calls that do
// not concern the back buffer pass on to the real swap chain.
class SwapChain final : public IDXGISwapChain4
{
public:
    SwapChain(IDXGIFactory* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC& requested);
    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGIObject and IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override { return chain->SetPrivateData(name, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* data) override { return chain->SetPrivateDataInterface(name, data); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override { return chain->GetPrivateData(name, size, data); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID id, void** parent) override { return chain->GetParent(id, parent); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void** device) override { return device11->QueryInterface(id, device); }

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT interval, UINT flags) override { return present(interval, flags); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID id, void** surface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override { return chain->SetFullscreenState(fullscreen, target); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override { return chain->GetFullscreenState(fullscreen, target); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* result) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* mode) override { return chain->ResizeTarget(mode); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override { return chain->GetContainingOutput(output); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* statistics) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override;

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* result) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* result) override { return chain->GetFullscreenDesc(result); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* result) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID id, void** result) override { return chain->GetCoreWindow(id, result); }
    // Every present copies the whole frame, so dirty rectangles and scrolling do not apply.
    HRESULT STDMETHODCALLTYPE Present1(UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS*) override { return present(interval, flags); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return chain->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override { return chain->GetRestrictToOutput(output); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override { return chain->SetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override { return chain->GetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override { return chain->SetRotation(rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override { return chain->GetRotation(rotation); }

    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override { return chain->SetSourceSize(width, height); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override { return chain->GetSourceSize(width, height); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override { return chain->SetMaximumFrameLatency(latency); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override { return chain->GetMaximumFrameLatency(latency); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return chain->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override { return chain->SetMatrixTransform(matrix); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override { return chain->GetMatrixTransform(matrix); }

    // IDXGISwapChain3. The game sees one back buffer, as Direct3D 11 shows the current one as buffer 0.
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return 0; }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space, UINT* support) override
    {
        return chain->CheckColorSpaceSupport(space, support);
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE space) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT*,
        IUnknown* const*) override
    {
        return ResizeBuffers(count, width, height, format, flags);
    }

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* metadata) override
    {
        return chain->SetHDRMetaData(type, size, metadata);
    }

private:
    ~SwapChain();

    HRESULT present(UINT interval, UINT flags);
    void copyFrame(const frame_generation::Inputs& inputs);
    void createBackBuffer();
    void waitIdle();
    void waitFor(UINT64 value);

    std::atomic<ULONG> references{1};

    // Direct3D 11, native: ReShade passes its proxy's device on unwrapped.
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Fence> fence11;

    // Direct3D 12, native: a device ReShade wraps would show other add-ons a second game device.
    // Streamline's proxies of the device and queue make its swap chain. The copies run on a queue of
    // their own: DLSS Frame Generation blocks the presenting queue while it works, and Direct3D 11's
    // next frame waits only for its copy, not for that. FSR generates on the queue it presents from,
    // after the copy and the preparation it needs, so for FSR that is the copy queue.
    ComPtr<ID3D12Device> wrappedDevice, device12, streamlineDevice;
    ComPtr<ID3D12CommandQueue> streamlineQueue, presentQueue, copyQueue;
    ComPtr<ID3D12Fence> fence12;
    std::array<ComPtr<ID3D12CommandAllocator>, FramesInFlight> allocators;
    std::array<UINT64, FramesInFlight> submitted{};
    ComPtr<ID3D12GraphicsCommandList> list;
    std::unique_ptr<void, HandleCloser> fenceEvent;
    UINT64 fenceValue{}, frames{};
    // The game's own presents and when the last one was handed on, for the counts it is shown.
    std::atomic<UINT> gamePresents{};
    std::atomic<LONGLONG> lastPresentTime{};

    // Streamline's proxy of the real swap chain, or AMD's swap chain.
    ComPtr<IDXGISwapChain4> chain;
    HWND window{};
    DXGI_SWAP_CHAIN_DESC desc{}; // As the game sees it
    DXGI_COLOR_SPACE_TYPE colorSpace{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};

    // The game renders here; Direct3D 12 copies from its view of the same texture.
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D12Resource> sharedBackBuffer;
    OpenedTexture depth, motion, hudless;
};

SwapChain::SwapChain(IDXGIFactory* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC& requested)
    : window(requested.OutputWindow), desc(requested)
{
    device11 = unwrapped<ID3D11Device5>(device);
    if (!device11) throw std::runtime_error("The game's device lacks Direct3D 11.4.");

    ComPtr<ID3D11DeviceContext> context;
    device11->GetImmediateContext(context.GetAddressOf());
    check(context.As(&context11), "Finding the Direct3D 11.4 context");
    check(device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)), "Creating the shared fence");

    const auto d3d12 = createDevice12(device11.Get());
    wrappedDevice = d3d12.wrapper;
    device12 = d3d12.native;
    const D3D12_COMMAND_QUEUE_DESC queueDesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (presentRoute == Route::Streamline)
    {
        if (!frame_generation::attach(device12.Get())) throw std::runtime_error("Streamline did not take the Direct3D 12 device.");

        streamlineDevice = upgraded<ID3D12Device>(device12.Get(), "the Direct3D 12 device");
        check(streamlineDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&streamlineQueue)), "Creating the Direct3D 12 queue");
        ComPtr<IUnknown> nativeQueue;
        nativeQueue.Attach(frame_generation::nativeOf(streamlineQueue.Get()));
        if (!nativeQueue || FAILED(nativeQueue.As(&presentQueue))) throw std::runtime_error("Finding the native Direct3D 12 queue failed.");
    }

    check(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&copyQueue)), "Creating the Direct3D 12 copy queue");
    if (presentRoute == Route::Fsr) presentQueue = copyQueue;
    HANDLE sharedFence{};
    check(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedFence), "Sharing the fence");
    check(openSharedHandle(device12.Get(), sharedFence, fence12), "Opening the shared fence");

    for (auto& allocator : allocators)
        check(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Creating a command allocator");

    check(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list)),
        "Creating the command list");
    check(list->Close(), "Closing the command list");
    fenceEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!fenceEvent) throw std::runtime_error("Creating the fence event failed.");

    // Direct3D 12 presents only with flip model, and the copies need nothing beyond output. Through
    // Streamline's factory the real swap chain is Streamline's to present; AMD's runtime makes its own.
    DXGI_SWAP_CHAIN_DESC1 real{};
    real.Width = desc.BufferDesc.Width;
    real.Height = desc.BufferDesc.Height;
    real.Format = presentable(desc.BufferDesc.Format);
    real.SampleDesc = {1, 0};
    real.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    real.BufferCount = std::max(desc.BufferCount, 2u);
    real.Scaling = DXGI_SCALING_STRETCH;
    real.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    real.Flags = desc.Flags;
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{desc.BufferDesc.RefreshRate, desc.BufferDesc.ScanlineOrdering, desc.BufferDesc.Scaling,
        desc.Windowed};
    ComPtr<IDXGIFactory2> factory2;
    check(factory->QueryInterface(IID_PPV_ARGS(&factory2)), "Finding IDXGIFactory2");
    if (presentRoute == Route::Streamline)
    {
        const auto streamlineFactory = upgraded<IDXGIFactory2>(factory2.Get(), "the DXGI factory");
        ComPtr<IDXGISwapChain1> created;
        check(streamlineFactory->CreateSwapChainForHwnd(streamlineQueue.Get(), window, &real, &fullscreen, nullptr, created.GetAddressOf()),
            "Creating the Direct3D 12 swap chain");
        check(created.As(&chain), "Finding IDXGISwapChain4");
    }
    else chain.Attach(fsr_frame_generation::createSwapChain(factory2.Get(), presentQueue.Get(), window, real, &fullscreen));

    // A width or height of 0 takes the window's.
    DXGI_SWAP_CHAIN_DESC1 actual{};
    check(chain->GetDesc1(&actual), "Reading the swap chain's size");
    desc.BufferDesc.Width = actual.Width;
    desc.BufferDesc.Height = actual.Height;
    createBackBuffer();
}

SwapChain::~SwapChain()
{
    try
    {
        waitIdle();
    }
    catch (const Failure& failure)
    {
        logFailure(failure);
    }

    // DXGI refuses to release a swap chain in exclusive fullscreen.
    BOOL fullscreen{};
    if (SUCCEEDED(chain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen) chain->SetFullscreenState(FALSE, nullptr);

    // Streamline's proxies go before Streamline, and Streamline before the devices it was given. FSR's
    // contexts go before its swap chain.
    if (presentRoute == Route::Fsr) fsr_frame_generation::stop();
    chain.Reset();
    streamlineQueue.Reset();
    streamlineDevice.Reset();
    if (presentRoute == Route::Streamline) frame_generation::stop();
    presenting = false;
    setStatus("The game released its swap chain.");
}

HRESULT SwapChain::QueryInterface(REFIID id, void** object)
{
    if (!object) return E_POINTER;

    if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIObject) || id == __uuidof(IDXGIDeviceSubObject) || id == __uuidof(IDXGISwapChain)
        || id == __uuidof(IDXGISwapChain1) || id == __uuidof(IDXGISwapChain2) || id == __uuidof(IDXGISwapChain3)
        || id == __uuidof(IDXGISwapChain4))
    {
        AddRef();
        *object = static_cast<IDXGISwapChain4*>(this);

        return S_OK;
    }

    *object = nullptr;

    return E_NOINTERFACE;
}

ULONG SwapChain::Release()
{
    const auto left = --references;
    if (!left) delete this;

    return left;
}

HRESULT SwapChain::GetBuffer(UINT index, REFIID id, void** surface)
{
    if (!surface) return DXGI_ERROR_INVALID_CALL;

    *surface = nullptr;
    if (index != 0 || !backBuffer) return DXGI_ERROR_INVALID_CALL;

    return backBuffer->QueryInterface(id, surface);
}

HRESULT SwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* result)
{
    if (!result) return DXGI_ERROR_INVALID_CALL;

    BOOL fullscreen{};
    if (const auto status = chain->GetFullscreenState(&fullscreen, nullptr); FAILED(status)) return status;

    *result = desc;
    result->Windowed = !fullscreen;

    return S_OK;
}

HRESULT SwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* result)
{
    if (!result) return DXGI_ERROR_INVALID_CALL;

    *result = {};
    result->Width = desc.BufferDesc.Width;
    result->Height = desc.BufferDesc.Height;
    result->Format = desc.BufferDesc.Format;
    result->SampleDesc = desc.SampleDesc;
    result->BufferUsage = desc.BufferUsage;
    result->BufferCount = desc.BufferCount;
    result->Scaling = DXGI_SCALING_STRETCH;
    result->SwapEffect = desc.SwapEffect;
    result->Flags = desc.Flags;

    return S_OK;
}

HRESULT SwapChain::GetHwnd(HWND* result)
{
    if (!result) return DXGI_ERROR_INVALID_CALL;

    *result = window;

    return S_OK;
}

HRESULT SwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE space)
{
    const auto result = chain->SetColorSpace1(space);
    if (SUCCEEDED(result)) colorSpace = space;

    return result;
}

// Frame generation presents several times per game frame, so the real swap chain's counts are not
// the game's (Days Gone 1.07 timed its frames by them). The counts are the game's own presents, each
// shown once handed on.
HRESULT SwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* statistics)
{
    if (!statistics) return DXGI_ERROR_INVALID_CALL;

    if (FAILED(chain->GetFrameStatistics(statistics))) *statistics = {};
    statistics->PresentCount = gamePresents;
    statistics->SyncQPCTime.QuadPart = lastPresentTime;

    return S_OK;
}

HRESULT SwapChain::GetLastPresentCount(UINT* count)
{
    if (!count) return DXGI_ERROR_INVALID_CALL;

    *count = gamePresents;

    return S_OK;
}

HRESULT SwapChain::ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    try
    {
        // Direct3D 12 resizes only once nothing uses the old buffers. ReShade has released its own.
        waitIdle();
        if (presentRoute == Route::Fsr) fsr_frame_generation::release();
        backBuffer.Reset();
        sharedBackBuffer.Reset();
        const auto result = chain->ResizeBuffers(count ? std::max(count, 2u) : 0, width, height, presentable(format), flags);
        if (SUCCEEDED(result))
        {
            DXGI_SWAP_CHAIN_DESC1 actual{};
            check(chain->GetDesc1(&actual), "Reading the swap chain's size");
            desc.BufferDesc.Width = actual.Width;
            desc.BufferDesc.Height = actual.Height;
            if (count) desc.BufferCount = count;
            if (format != DXGI_FORMAT_UNKNOWN) desc.BufferDesc.Format = format;
            desc.Flags = flags;
        }

        // The game gets a back buffer either way, at the old size when resizing failed.
        createBackBuffer();

        return result;
    }
    catch (const Failure& failure)
    {
        logFailure(failure);

        return failure.result;
    }
}

HRESULT SwapChain::present(UINT interval, UINT flags)
{
    // A test present only asks whether the window is visible; nothing was rendered for it.
    if (flags & DXGI_PRESENT_TEST) return chain->Present(interval, flags);

    // ReShade has drawn its overlay by now, after the add-on's present event submitted the frame.
    const auto inputs = std::exchange(submittedFrame, std::nullopt).value_or(frame_generation::Inputs{});
    try
    {
        copyFrame(inputs);
    }
    catch (const Failure& failure)
    {
        logFailure(failure);

        return failure.result;
    }

    frame_generation::beginPresent(inputs);
    const auto result = chain->Present(interval, flags);
    LARGE_INTEGER presented{};
    QueryPerformanceCounter(&presented);
    lastPresentTime = presented.QuadPart;
    ++gamePresents;
    frame_generation::endPresent(inputs);

    return result;
}

void SwapChain::copyFrame(const frame_generation::Inputs& inputs)
{
    const auto rendered = ++fenceValue;
    check(context11->Signal(fence11.Get(), rendered), "Signalling the rendered frame");
    context11->Flush();

    const auto slot = frames++ % FramesInFlight;
    waitFor(submitted[slot]);
    check(allocators[slot]->Reset(), "Resetting a command allocator");
    check(list->Reset(allocators[slot].Get(), nullptr), "Resetting the command list");
    // Streamline chooses the back buffer; frame generation keeps some of them.
    ComPtr<ID3D12Resource> target;
    check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target)), "Finding the swap chain's back buffer");
    D3D12_RESOURCE_BARRIER barriers[]{
        transition(sharedBackBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    list->ResourceBarrier(2, barriers);
    list->CopyResource(target.Get(), sharedBackBuffer.Get());
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(2, barriers);
    try
    {
        ID3D12Resource *depthResource{}, *motionResource{}, *hudlessResource{};
        if (inputs.valid)
        {
            depthResource = openInput(device12.Get(), depth, inputs.depth);
            motionResource = openInput(device12.Get(), motion, inputs.motion);
            hudlessResource = openInput(device12.Get(), hudless, inputs.hudless);
        }

        // FSR also hears of frames without inputs, to stop generating.
        if (presentRoute == Route::Fsr)
            fsr_frame_generation::record(list.Get(), inputs, depthResource, motionResource, hudlessResource,
                colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        else if (inputs.valid) frame_generation::tag(list.Get(), inputs, depthResource, motionResource, hudlessResource);
    }
    catch (const Failure& failure)
    {
        logFailure(failure);
    }

    check(list->Close(), "Recording the copy");

    check(copyQueue->Wait(fence12.Get(), rendered), "Waiting for the rendered frame");
    ID3D12CommandList* lists[]{list.Get()};
    copyQueue->ExecuteCommandLists(1, lists);
    const auto copied = ++fenceValue;
    check(copyQueue->Signal(fence12.Get(), copied), "Signalling the copied frame");
    submitted[slot] = copied;
    // The game's next frame draws into the same textures; the frame is presented once copied.
    check(context11->Wait(fence11.Get(), copied), "Waiting for the copied frame");
    check(presentQueue->Wait(fence12.Get(), copied), "Waiting to present the copied frame");
}

void SwapChain::createBackBuffer()
{
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = desc.BufferDesc.Width;
    texture.Height = desc.BufferDesc.Height;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = desc.BufferDesc.Format;
    texture.SampleDesc.Count = 1;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (desc.BufferUsage & DXGI_USAGE_SHADER_INPUT) texture.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) texture.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    check(device11->CreateTexture2D(&texture, nullptr, backBuffer.ReleaseAndGetAddressOf()), "Creating the shared back buffer");
    sharedBackBuffer = openTexture(device12.Get(), backBuffer.Get(), "Opening the shared back buffer");
}

// Both devices and both queues finish everything given so far.
void SwapChain::waitIdle()
{
    const auto rendered = ++fenceValue;
    check(context11->Signal(fence11.Get(), rendered), "Signalling Direct3D 11's work");
    context11->Flush();
    check(copyQueue->Wait(fence12.Get(), rendered), "Waiting for Direct3D 11's work");
    const auto copied = ++fenceValue;
    check(copyQueue->Signal(fence12.Get(), copied), "Signalling the copy queue's work");
    check(presentQueue->Wait(fence12.Get(), copied), "Waiting for the copy queue's work");
    const auto done = ++fenceValue;
    check(presentQueue->Signal(fence12.Get(), done), "Signalling Direct3D 12's work");
    waitFor(done);
}

void SwapChain::waitFor(UINT64 value)
{
    if (fence12->GetCompletedValue() >= value) return;

    check(fence12->SetEventOnCompletion(value, fenceEvent.get()), "Waiting for Direct3D 12");
    if (WaitForSingleObject(fenceEvent.get(), FenceTimeout) != WAIT_OBJECT_0)
        throw Failure("Direct3D 12 did not finish a frame within 10 seconds.", DXGI_ERROR_DEVICE_HUNG);
}

// Special K under any file name: SpecialK64.dll when injected, dxgi.dll or d3d11.dll beside the game.
bool specialKLoaded() noexcept
{
    HMODULE modules[1024]{};
    DWORD bytes{};
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return false;

    const auto count = std::min<std::size_t>(bytes / sizeof(HMODULE), std::size(modules));
    return std::any_of(modules, modules + count, [](HMODULE module) { return GetProcAddress(module, "SK_GetDLLRole") != nullptr; });
}

// DXGI's own function behind an entry of its factories' function table. Some setups replace the entry
// in memory (ReShade does when Ubisoft's overlay is loaded) and call the function it held, so it is
// read from the entry in dxgi.dll's file and relocated to where dxgi.dll is loaded.
void* originalEntry(void* const* slot)
{
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(slot), &module))
        throw std::runtime_error("Finding the module that holds DXGI's function table failed.");

    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) throw std::runtime_error("Finding dxgi.dll's file failed.");

    std::ifstream file(path, std::ios::binary);
    const std::vector<char> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto read = [&](std::size_t offset, auto& value)
    {
        if (offset > image.size() || image.size() - offset < sizeof(value)) throw std::runtime_error("dxgi.dll's file is shorter than its headers say.");
        std::memcpy(&value, image.data() + offset, sizeof(value));
    };
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    read(0, dos);
    read(static_cast<std::size_t>(dos.e_lfanew), nt);
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* loaded = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(module)->e_lfanew);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt.FileHeader.TimeDateStamp != loaded->FileHeader.TimeDateStamp)
        throw std::runtime_error("dxgi.dll's file is not the one loaded.");

    const auto rva = reinterpret_cast<std::uintptr_t>(slot) - base;
    const auto sections = static_cast<std::size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i)
    {
        IMAGE_SECTION_HEADER section{};
        read(sections + i * sizeof(section), section);
        if (rva < section.VirtualAddress || rva + sizeof(std::uint64_t) > section.VirtualAddress + section.SizeOfRawData) continue;

        std::uint64_t stored{};
        read(section.PointerToRawData + (rva - section.VirtualAddress), stored);
        const auto entry = static_cast<std::uintptr_t>(stored - nt.OptionalHeader.ImageBase) + base;
        if (entry < base || entry >= base + loaded->OptionalHeader.SizeOfImage) break;

        return reinterpret_cast<void*>(entry);
    }

    throw std::runtime_error("DXGI's CreateSwapChain is not in dxgi.dll's file.");
}

// Behind ReShade: ReShade wraps what this returns as the game's swap chain.
HRESULT STDMETHODCALLTYPE createSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain)
{
    // Only the game's Direct3D 11 swap chain is replaced; any other while it lives passes on, and so do
    // those other tools make from their own threads (Special K makes one before the game does).
    ComPtr<ID3D11Device> game;
    if (presenting || !device || !desc || !chain || FAILED(device->QueryInterface(IID_PPV_ARGS(&game))) || !game_code::calledFromGame())
        return originalCreate(factory, device, desc, chain);

    // Without frame generation the game's own swap chain does the same job without the copy.
    const bool fsr = presentRoute == Route::Fsr;
    bool started{};
    if (fsr) started = fsr_frame_generation::start(addonDirectory);
    else
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        started = SUCCEEDED(game.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))
            && frame_generation::start(addonDirectory, adapter.Get());
    }

    if (!started)
    {
        setStatus("Frame generation did not start: " + (fsr ? fsr_frame_generation::status() : frame_generation::status()));

        return originalCreate(factory, device, desc, chain);
    }

    SwapChain* created{};
    try
    {
        created = new SwapChain(factory, device, *desc);
    }
    catch (const std::exception& error)
    {
        if (fsr) fsr_frame_generation::stop();
        else frame_generation::stop();

        setStatus(std::string("Frame generation did not start: ") + error.what());

        return originalCreate(factory, device, desc, chain);
    }

    *chain = created;
    presenting = true;
    setStatus("Presenting through Direct3D 12 for frame generation.");

    return S_OK;
}
}

// ReShade loads the add-on again when the game makes a new device; the pinned module keeps this
// state, but the hook went with MH_Uninitialize, so each load hooks again. A stand-in still alive
// keeps the route it was made on.
void install(const std::filesystem::path& directory, Route route) noexcept
{
    hooked = true;
    addonDirectory = directory;
    if (!presenting) presentRoute = route;
    // Special K's present hook crashes on the Direct3D 12 swap chain that frame generation presents
    // through, and with that hook off DLSS Frame Generation crashes.
    if (specialKLoaded())
    {
        setStatus("Frame generation does not work with Special K yet.");
        return;
    }

    try
    {
        // ReShade gives the game proxies of DXGI's factories, which call the real factories' shared
        // function table. Detouring DXGI's function behind that table puts this hook behind ReShade, so
        // ReShade wraps the stand-in as it would the game's own swap chain: its overlay, effects and
        // add-on events see a Direct3D 11 game, which add-ons such as RenoDX's DLSS 5 need to follow DLSS.
        ComPtr<IDXGIFactory> proxy;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&proxy)), "Creating a DXGI factory");
        const auto factory = unwrapped<IDXGIFactory>(proxy.Get());
        if (!factory) throw std::runtime_error("The DXGI factory lacks IDXGIFactory.");

        auto* target = originalEntry(&(*reinterpret_cast<void***>(factory.Get()))[CreateSwapChainSlot]);
        if (const auto status = game_code::hookAt(target, &createSwapChain, originalCreate); status != MH_OK)
            throw std::runtime_error(std::string("Hooking DXGI's CreateSwapChain failed: ") + MH_StatusToString(status) + ".");

        setStatus("Waiting for the game's swap chain.");
    }
    catch (const std::exception& error)
    {
        setStatus(std::string("Frame generation did not start: ") + error.what());
    }
}

void submit(const frame_generation::Inputs& inputs) noexcept
{
    submittedFrame = inputs;
}

bool installed() noexcept
{
    return hooked;
}

bool active() noexcept
{
    return presenting;
}

Route route() noexcept
{
    return presentRoute;
}

std::string status()
{
    std::lock_guard lock(statusMutex);
    return statusText;
}

unsigned presentedPerFrame() noexcept
{
    if (!presenting) return 1;

    return presentRoute == Route::Fsr ? fsr_frame_generation::presentedPerFrame() : frame_generation::presentedPerFrame();
}
}
