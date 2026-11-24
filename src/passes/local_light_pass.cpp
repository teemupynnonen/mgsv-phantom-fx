#include "passes/local_light_pass.hpp"

#include "d3d11_helpers.hpp"
#include "features/lamp_contact_shadows/lamp_contact_shadows.hpp"
#include "features/lamp_soft_shadows/lamp_soft_shadows.hpp"
#include "frame_latch.hpp"
#include "game_lights.hpp"
#include "log.hpp"
#include "passes/contact_shadow_trace.hpp"
#include "performance.hpp"

#include <wrl/client.h>
#include <array>
#include <atomic>
#include <optional>
#include <stdexcept>
#include <string>

namespace phantom_fx::local_light_pass
{
using fxcore::FrameLatch;

namespace
{
using Microsoft::WRL::ComPtr;
using namespace game_lights;
using game_shaders::GamePass, game_shaders::GameShader, game_shaders::GameShaders;

// The lamp shadow effects the pass adds, by the effect plan.
struct Composition
{
    bool contactShadows{}, softShadows{};

    bool any() const { return contactShadows || softShadows; }
};

FrameLatch<Composition> latch;
std::atomic<bool> active{};

// Whether the log has named a light shader's first lighting draw, by its row.
std::array<std::atomic<bool>, std::size(GameShaders)> named{};

// A light shader's kind, as the log names it.
std::string kindOf(const GameLight& light)
{
    return std::string(light.shape == Shape::Point ? "point light" : light.flashlight ? "flashlight" : "spot light")
        + (light.falloff == Falloff::Quartic ? ", quartic falloff" : ", sixteenth falloff") + (light.specular ? ", specular" : ", no specular")
        + (light.shadowMap ? ", shadow map" : ", no shadow map");
}

// A light's draws: the marking draw (colour writes off) takes the contact shadow; the lighting draw
// the contact shadow's blend and the soft shadows' shader.
bool drawLight(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const GameShader& shader, ID3D11PixelShader* gameShader,
    const Composition& composition)
{
    const auto gameBlend = boundBlend(context);
    if (!gameBlend.state) return false;

    D3D11_BLEND_DESC blend{};
    gameBlend.state->GetDesc(&blend);
    if (blend.RenderTarget[0].RenderTargetWriteMask == 0)
        return composition.contactShadows && lamp_contact_shadows::drawMarking(context, call, shader.light->falloff, gameBlend);

    const auto replacement = composition.softShadows ? lamp_soft_shadows::replacementFor(*shader.light) : std::nullopt;

    if (!named[static_cast<std::size_t>(&shader - std::begin(GameShaders))].exchange(true))
        log("Shadows: the game draws a " + kindOf(*shader.light) + (replacement ? ", with the soft shadows." : "."));

    auto* shadowed = composition.contactShadows ? lamp_contact_shadows::takeShadowedBlend(context, gameBlend.state.Get()) : nullptr;
    if (!replacement && !shadowed) return false;

    if (shadowed) context->OMSetBlendState(shadowed, gameBlend.factor, gameBlend.sampleMask);

    constexpr UINT OptionsSlot = lamp_soft_shadows::Replacement::OptionsSlot;
    ComPtr<ID3D11Buffer> gameOptions;
    if (replacement)
    {
        context->PSGetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
        context->PSSetShader(replacement->shader, nullptr, 0);
        context->PSSetConstantBuffers(OptionsSlot, 1, &replacement->options);
    }

    performance::Timed timed(context, replacement ? performance::Pass::LampSoftLight : performance::Pass::LampShadows);
    context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    timed.end();

    if (shadowed) context->OMSetBlendState(gameBlend.state.Get(), gameBlend.factor, gameBlend.sampleMask);

    if (replacement)
    {
        context->PSSetShader(gameShader, nullptr, 0);
        context->PSSetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
        logOnce("Shadows: the lamps draw with soft shadows.");
    }

    return true;
}
}

void configure(const Configuration& configuration) noexcept
{
    latch.set({.contactShadows = configuration.effects.runs(Effect::LampContactShadows), .softShadows = configuration.effects.runs(Effect::LampSoftShadows)});
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!active.load(std::memory_order_relaxed) || !call.indexed || call.instances != 1) return false;

    try
    {
        const auto* light = shader.row();
        if (!light || light->pass != GamePass::LocalLight) return false;

        return drawLight(context, call, *light, shader.object(), latch.frame());
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("Shadows: a light pass kept the game's shadows: ") + error.what());
    }
    catch (...)
    {
    }

    return false;
}

void present() noexcept
{
    const auto current = latch.present();
    active = current.any();
    contact_shadow_trace::tracesThisFrame(contact_shadow_trace::Tracer::Lamps, current.contactShadows);
}
}
