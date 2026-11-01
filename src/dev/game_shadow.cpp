#include "dev/game_shadow.hpp"

#include "d3d11_helpers.hpp"
#include "log.hpp"
#include "passes/sun_light_pass_shaders.hpp"
#include "resource_dump.hpp"

#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>

namespace phantom_fx::game_shadow
{
namespace
{
using Microsoft::WRL::ComPtr;

std::atomic<bool> keeps{};
// The shader that draws the shadow map's answer, its target, and the depth state it draws with, made from `depthStateOf`,
// the sun pass's.
ComPtr<ID3D11PixelShader> shader;
ComPtr<ID3D11RenderTargetView> target;
ComPtr<ID3D11DepthStencilState> depthState, depthStateOf;
// The answer. Under its own lock, held across no Direct3D call: the present saves it, and it holding Direct3D 11's lock
// while waiting for the sun pass's lock, held by a sun pass waiting for Direct3D 11's, froze the game (2026-10-30, both
// threads' stacks read from the hung process).
std::mutex answerMutex;
ComPtr<ID3D11Texture2D> answer;

// Before the pass's own draw and leaving the stencil as it is: the pass zeroes the bit it tests as it draws (read
// 2026-10-30: pass op zero, write mask 8), so a draw after it found nothing. Pixels the pass skips stay at -1.
void drawAnswer(ID3D11Device* device, ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, UINT width, UINT height)
{
    if (!shader)
    {
        const D3D_SHADER_MACRO defines[]{{"SHADOW_MAP", "1"}, {"SKY_REFLECTION", "0"}, {"GAME_SHADOW", "1"}, {}};
        shader = pixelShader(device, sun_light_pass::shaders::SunLight, "sun-light-game-shadow", defines);
    }

    ComPtr<ID3D11Texture2D> texture;
    {
        std::lock_guard lock(answerMutex);
        texture = answer;
    }

    D3D11_TEXTURE2D_DESC desc{};
    if (texture) texture->GetDesc(&desc);
    if (desc.Width != width || desc.Height != height)
    {
        desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        texture.Reset();
        target.Reset();
        check(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()), "Creating the game's shadow copy");
        check(device->CreateRenderTargetView(texture.Get(), nullptr, target.GetAddressOf()), "Creating the game's shadow target");
        std::lock_guard lock(answerMutex);
        answer = texture;
    }

    ComPtr<ID3D11RenderTargetView> targets[2];
    ComPtr<ID3D11DepthStencilView> depthStencil;
    context->OMGetRenderTargets(2, targets[0].GetAddressOf(), depthStencil.GetAddressOf());
    ComPtr<ID3D11BlendState> blend;
    FLOAT blendFactor[4]{};
    UINT sampleMask{};
    context->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
    ComPtr<ID3D11DepthStencilState> passDepthState;
    UINT stencilReference{};
    context->OMGetDepthStencilState(passDepthState.GetAddressOf(), &stencilReference);
    if (!depthState || depthStateOf != passDepthState)
    {
        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        if (passDepthState) passDepthState->GetDesc(&depthDesc);
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depthDesc.StencilWriteMask = 0;
        depthState.Reset();
        check(device->CreateDepthStencilState(&depthDesc, depthState.GetAddressOf()), "Creating the game's shadow depth state");
        depthStateOf = passDepthState;
    }

    constexpr FLOAT NoAnswer[4]{-1, -1, -1, -1};
    context->ClearRenderTargetView(target.Get(), NoAnswer);
    context->OMSetRenderTargets(1, target.GetAddressOf(), depthStencil.Get());
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(depthState.Get(), stencilReference);
    context->PSSetShader(shader.Get(), nullptr, 0);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);

    ID3D11RenderTargetView* const restored[]{targets[0].Get(), targets[1].Get()};
    context->OMSetRenderTargets(2, restored, depthStencil.Get());
    context->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
    context->OMSetDepthStencilState(passDepthState.Get(), stencilReference);
}
}

void keep(bool isKept) noexcept
{
    if (isKept)
    {
        // Only this capture's sun passes may give the reference: indoors there are none.
        std::lock_guard lock(answerMutex);
        answer.Reset();
    }

    keeps = isKept;
}

bool isKept() noexcept
{
    return keeps.load(std::memory_order_relaxed);
}

void draw(ID3D11Device* device, ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, UINT width, UINT height) noexcept
{
    try
    {
        drawAnswer(device, context, call, width, height);
    }
    catch (const std::exception& error)
    {
        keeps = false;
        logOnce(std::string("Debug case: the game's shadow could not be kept: ") + error.what());
    }
}

void save(ID3D11DeviceContext* context, const std::filesystem::path& file) noexcept
{
    ComPtr<ID3D11Texture2D> texture;
    {
        std::lock_guard lock(answerMutex);
        texture = answer;
    }

    if (texture) saveTexture(context, texture.Get(), DXGI_FORMAT_UNKNOWN, file);
}

void detach() noexcept
{
    shader.Reset();
    target.Reset();
    depthState.Reset();
    depthStateOf.Reset();
    std::lock_guard lock(answerMutex);
    answer.Reset();
}
}
