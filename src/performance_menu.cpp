// The costs in the settings window's budget page, over the game, and as text for PhantomFX-performance.txt.
#include "performance.hpp"

#include "core.hpp"
#include "features/upscaling/upscaler.hpp"
#include "fxui/fxui.hpp"
#include "menu_budget.hpp"

#include <Windows.h>
#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <algorithm>
#include <format>
#include <iterator>

namespace phantom_fx::performance
{
namespace
{
// What the last measurement found each effect really costs, everything included; with `explained`, how.
void drawRealCost(const Measurement& m, bool explained)
{
    if (m.steps.size() < 2)
    {
        if (!m.running) fxui::note(fxui::Tone::Dim, "Not measured yet.");
        return;
    }

    const auto minutes = (GetTickCount64() - m.finishedAt) / 60000;
    const auto age = minutes ? std::format("{} min ago", minutes) : std::string("just now");
    if (explained) fxui::note(fxui::Tone::Dim, std::format("GPU ms saved by leaving each effect out, measured {}. It includes what the timed passes miss.", age));
    fxui::costRow("GPU frame with everything on", m.steps.front().gpuFrame, 0);
    for (std::size_t i = 1; i < m.steps.size(); ++i) fxui::costRow(m.steps[i].removed, m.steps[i - 1].gpuFrame - m.steps[i].gpuFrame);
    fxui::costRow("All of these", m.steps.front().gpuFrame - m.steps.back().gpuFrame, 0);
}

// The effects switched together; neither lights up while they are switched otherwise.
void drawPresets(Settings& wanted)
{
    constexpr EffectPreset presets[]{EffectPreset::ScreenSpace, EffectPreset::RayTraced};
    constexpr fxui::Option options[]{
        {.label = "Screen-space",
            .tip = "Soft shadows, sun and lamp contact shadows, GTVBAO and VB-SSGI. No ray tracing, so it runs on any GPU."},
        {.label = "Ray-traced",
            .tip = "Ray-traced shadows, lights and global illumination in place of the screen-space effects. Needs a GPU that traces rays (DXR 1.1)."},
    };
    const auto current = presetOf(wanted);
    const int selected = current ? static_cast<int>(std::ranges::find(presets, *current) - std::begin(presets)) : -1;
    const int pressed = fxui::segmented("Preset", selected, options,
        "Switches the effects in every section at once. Both also unlock the frame rate and turn on the volumetric fog in the physical look. "
        "The upscaler, HDR and each effect's own settings stay as they are.");
    if (pressed >= 0) applyPreset(wanted, presets[pressed]);
}
}

void drawBudget(Settings& wanted)
{
    const fxui::Page page(Section::Budget);
    if (!page) return;

    drawPresets(wanted);
    fxui::gap(8);

    const auto state = status();
    if (fxui::button("Measure the real cost",
            "Times the GPU frame with everything on, then leaves out one effect at a time, the upscaler last: about 4 seconds a step. The differences "
            "include what the timed passes miss, such as the game's SSAO that GTVBAO leaves out. Hold still (photo mode) for steady numbers; your "
            "settings come back at the end. HDR takes a restart, so it is not measured.",
            !state.measurement.running))
        measure();
    ImGui::SameLine(0, ImGui::GetFontSize() * 1.25f);
    bool shown = state.overlay;
    if (fxui::checkbox("Show the costs over the game", shown,
            "Keeps the costs in the top right corner, also with this window closed. PhantomFX-performance.txt beside the log holds the same numbers "
            "meanwhile, rewritten every second."))
        showOverlay(shown);
    fxui::gap(12);
    if (state.measurement.running)
        fxui::note(fxui::Tone::Warn, std::format("Measuring the real cost: step {} of {}. Hold still.", state.measurement.step + 1, state.measurement.stepCount));

    fxui::ledger();
    fxui::subheading("Real cost");
    drawRealCost(state.measurement, true);
}

void drawOverlay()
{
    const auto state = status();
    if (!state.overlay) return;

    auto settings = core::currentSettings();
    const auto budget = menu::budget(settings, state.costs);
    if (const fxui::CostOverlay overlay(menu::sections(), budget, "Phantom FX"); overlay && state.measurement.steps.size() >= 2)
    {
        fxui::subheading("Real cost");
        drawRealCost(state.measurement, false);
    }
}

// A heading, the frame, one line per effect with its passes, and the real cost.
std::string report(const Status& state)
{
    auto settings = core::currentSettings();
    const auto budget = menu::budget(settings, state.costs);
    const auto& c = state.costs;
    std::string text = "Phantom FX GPU costs in milliseconds, averaged over about half a second. The timed passes miss what they change in "
                       "the game's own work; the real cost includes it.\n";
    text += std::format("Frame {:.2f} ms ({:.1f} fps); GPU frame {:.2f} ms; Phantom FX timed {:.2f} ms\n", c.frame, c.frame > 0 ? 1000 / c.frame : 0,
        c.gpuFrame, budget.total());
    for (const auto& line : budget.lines)
    {
        auto entry = std::format("{} {:.2f}:", line.name, line.ms);
        for (std::size_t i = 0; i < line.passes.size(); ++i) entry += std::format("{} {} {:.2f}", i ? "," : "", line.passes[i].name, line.passes[i].ms);

        text += entry + '\n';
    }

    const auto& m = state.measurement;
    if (m.running) text += std::format("Real cost: measuring, step {} of {}\n", m.step + 1, m.stepCount);
    if (m.steps.size() < 2)
    {
        if (!m.running) text += "Real cost: not measured\n";
        return text;
    }

    auto line = std::format("Real cost (GPU frame): everything on {:.2f}; leaving out", m.steps.front().gpuFrame);
    for (std::size_t i = 1; i < m.steps.size(); ++i)
        line += std::format("{} {} saves {:.2f}", i > 1 ? "," : "", m.steps[i].removed, m.steps[i - 1].gpuFrame - m.steps[i].gpuFrame);

    return text + line + std::format("; total {:.2f}\n", m.steps.front().gpuFrame - m.steps.back().gpuFrame);
}
}
