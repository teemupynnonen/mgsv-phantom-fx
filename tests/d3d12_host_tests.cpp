#include "features/upscaling/d3d12_host.hpp"

#include "d3d11_helpers.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using namespace phantom_fx;
using namespace phantom_fx::d3d12_host;
using Microsoft::WRL::ComPtr;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Direct3D 12 copies a shared input to a shared output between what Direct3D 11 writes and reads, a value a frame, so
// each side sees the other's work only if they really take turns.
class Bench
{
public:
    Bench() : device(warpDevice()), host(device.Get(), "the test")
    {
        device->GetImmediateContext(context.GetAddressOf());
        input = makeComputeTexture(device.Get(), Size, Size, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the input", SharedWithD3D12);
        output = makeComputeTexture(device.Get(), Size, Size, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the output", SharedWithD3D12);

        D3D11_TEXTURE2D_DESC desc{};
        output.texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()), "Creating the read-back");
    }

    void write(std::uint8_t value)
    {
        const std::array<float, 4> colour{value / 255.0f, 0, 0, 1};
        context->ClearUnorderedAccessViewFloat(input.access.Get(), colour.data());
    }

    void copy()
    {
        host.evaluate([&](Evaluation& evaluation) { copyIn(evaluation); });
    }

    std::uint8_t read()
    {
        context->CopyResource(staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Reading the output");
        const auto value = *static_cast<const std::uint8_t*>(mapped.pData);
        context->Unmap(staging.Get(), 0);

        return value;
    }

    Host& theHost() { return host; }

private:
    static constexpr UINT Size = 256;

    static ComPtr<ID3D11Device> warpDevice()
    {
        ComPtr<ID3D11Device> made;
        constexpr D3D_FEATURE_LEVEL Level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &Level, 1, D3D11_SDK_VERSION, made.GetAddressOf(), nullptr, nullptr)))
            throw std::runtime_error("No WARP device.");

        return made;
    }

    void copyIn(Evaluation& evaluation)
    {
        auto* from = evaluation.read(openedInput, input.texture.Get(), "Opening the input");
        auto* to = evaluation.write(openedOutput, output.texture.Get(), "Opening the output");
        D3D12_RESOURCE_BARRIER states[]{transition(from, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(to, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
        evaluation.list()->ResourceBarrier(2, states);
        evaluation.list()->CopyResource(to, from);
        for (auto& state : states) std::swap(state.Transition.StateBefore, state.Transition.StateAfter);
        evaluation.list()->ResourceBarrier(2, states);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Host host;
    ComputeTexture input, output;
    ComPtr<ID3D11Texture2D> staging;
    OpenedTexture openedInput, openedOutput;
};

// More frames than lists in flight, so the lists come round.
void theTwoSidesTakeTurns()
{
    Bench bench;
    for (int frame = 1; frame <= 32; ++frame)
    {
        const auto value = static_cast<std::uint8_t>(frame * 7);
        bench.write(value);
        bench.copy();
        expect(bench.read() == value, "Direct3D 11 reads what Direct3D 12 made of what it just wrote.");
    }
}

void aFailedEvaluationLeavesTheNextFine()
{
    Bench bench;
    bench.write(10);
    bench.copy();

    bool threw{};
    try
    {
        bench.theHost().evaluate([](Evaluation&) { throw std::runtime_error("The evaluation failed."); });
    }
    catch (const std::runtime_error& error)
    {
        threw = std::string(error.what()) == "The evaluation failed.";
    }
    expect(threw, "A failure reaches the caller.");

    bench.write(20);
    bench.copy();
    expect(bench.read() == 20, "The next evaluation runs.");
}

void waitingForTheLastLeavesNothingRunning()
{
    Bench bench;
    bench.theHost().waitForLast();

    bench.write(30);
    bench.copy();
    bench.theHost().waitForLast();
    expect(bench.read() == 30, "Waiting for the last evaluation returns once it is done.");
}

// Ray Reconstruction lets its old feature go inside the evaluation that makes the new one: that wait ends at this
// evaluation's inputs. WARP runs Direct3D 11 work unflushed, so this does not prove the host's flush.
void anEvaluationCanWaitForItsInputs()
{
    Bench bench;
    bench.write(40);
    bench.copy();

    bench.write(50);
    bench.theHost().evaluate([&](Evaluation&) { bench.theHost().waitForLast(); });
    bench.copy();
    expect(bench.read() == 50, "The wait ends, and the frame goes on.");
}
}

int main()
{
    try
    {
        theTwoSidesTakeTurns();
        aFailedEvaluationLeavesTheNextFine();
        waitingForTheLastLeavesNothingRunning();
        anEvaluationCanWaitForItsInputs();
        std::cout << "Direct3D 12 host tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
