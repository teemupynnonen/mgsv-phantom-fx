#include "passes/ray_tracing/shared_textures.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace phantom_fx::shared_textures;
using Microsoft::WRL::ComPtr;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

ComPtr<ID3D11Device> warpDevice()
{
    ComPtr<ID3D11Device> device;
    constexpr D3D_FEATURE_LEVEL Level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &Level, 1, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr)))
        throw std::runtime_error("No WARP device.");

    return device;
}

void nothingToReadUntilWritten(ID3D11Device* device)
{
    SharedOutput output(DXGI_FORMAT_R16_FLOAT, "Creating the test output");
    expect(!output.writtenView(64, 32), "Nothing is made yet.");

    auto* texture = output.ensure(device, 64, 32);
    expect(texture && !output.writtenView(64, 32), "Made is not written.");

    output.markWritten(texture);
    expect(output.writtenView(64, 32), "Written, at its size, it can be read.");
    expect(!output.writtenView(32, 32), "Another size reads nothing.");

    output.clearWritten();
    expect(!output.writtenView(64, 32), "A kick that traces nothing leaves nothing to read.");
}

void sameSizeKeepsTheTexture(ID3D11Device* device)
{
    SharedOutput output(DXGI_FORMAT_R16_FLOAT, "Creating the test output");
    auto* texture = output.ensure(device, 64, 32);
    output.markWritten(texture);
    expect(output.ensure(device, 64, 32) == texture && output.writtenView(64, 32), "The same size keeps the texture and what it holds.");
}

void newSizeLeavesItUnwritten(ID3D11Device* device)
{
    SharedOutput output(DXGI_FORMAT_R16_FLOAT, "Creating the test output");
    output.markWritten(output.ensure(device, 64, 32));

    output.ensure(device, 128, 64);
    expect(!output.writtenView(128, 64), "A new size holds nothing yet.");
}

// A reader of another size can replace it while a frame traces into the texture it had.
void replacedTextureIsNotWritten(ID3D11Device* device)
{
    SharedOutput output(DXGI_FORMAT_R16_FLOAT, "Creating the test output");
    ComPtr<ID3D11Texture2D> traced = output.ensure(device, 64, 32);

    output.ensure(device, 128, 64);
    output.markWritten(traced.Get());
    expect(!output.writtenView(128, 64), "The frame wrote the texture that was replaced.");
}

void resetGivesItBack(ID3D11Device* device)
{
    SharedOutput output(DXGI_FORMAT_R16_FLOAT, "Creating the test output");
    output.markWritten(output.ensure(device, 64, 32));

    output.reset();
    expect(!output.writtenView(64, 32), "Nothing is left to read.");
    expect(output.ensure(device, 64, 32) && !output.writtenView(64, 32), "It is made again, unwritten.");
}
}

int main()
{
    try
    {
        const auto device = warpDevice();
        nothingToReadUntilWritten(device.Get());
        sameSizeKeepsTheTexture(device.Get());
        newSizeLeavesItUnwritten(device.Get());
        replacedTextureIsNotWritten(device.Get());
        resetGivesItBack(device.Get());
        std::cout << "Shared textures tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
