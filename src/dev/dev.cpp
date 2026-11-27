#include "dev/dev.hpp"

#include "core.hpp"
#include "device_check.hpp"
#include "dev/devkit_link.hpp"
#include "dev/perf_record.hpp"
#include "dev/remaster/tool.hpp"
#include "devkit/cutscenes.hpp"
#include "devkit/game.hpp"
#include "devkit/game_lua.hpp"
#include "devkit/host.hpp"
#include "devkit/log.hpp"
#include "devkit/travel.hpp"
#include "features/photo_mode/photo_mode.hpp"
#include "fxui/fxui.hpp"
#include "log.hpp"
#include "performance.hpp"

#include <Windows.h>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

// The devkit's game layer: the build check the add-on makes.
namespace devkit::game
{
bool isSupportedBuild() noexcept
{
    return phantom_fx::isSupportedGameBuild();
}
}

namespace phantom_fx::dev
{
namespace
{
using fxui::Tone;

fxui::Tone toneOf(devkit::Tone tone)
{
    switch (tone)
    {
    case devkit::Tone::Dim: return fxui::Tone::Dim;
    case devkit::Tone::Ok: return fxui::Tone::Ok;
    case devkit::Tone::Warn: return fxui::Tone::Warn;
    case devkit::Tone::Bad: return fxui::Tone::Bad;
    }

    return fxui::Tone::Dim;
}

// What the light's rays cost, part by part: each part left out for a few seconds, after a few seconds with everything as
// it was, and the GPU's cost averaged over what is left after the settling. Run from the Light cache item, so it needs
// the Developer page open.
struct LightAblation
{
    static constexpr int Variants = 3, Phases = Variants * 2, PhaseMs = 6000, SettleMs = 2000;
    static constexpr const char* Names[Variants]{"the light cache", "the bounce rays", "the hits' own light"};
    bool running{};
    int phase{};
    std::uint64_t phaseStart{};
    Diagnostics base;
    double baseFrame{}, baseRays{}, baseCount{};
    double frame[Variants]{}, rays[Variants]{}, count[Variants]{};
    std::string result;
};

LightAblation ablation;

// The settings of phase `phase`: the variant that phase tests left out, everything else as it was.
Diagnostics ablatedSettings(int phase)
{
    auto settings = ablation.base;
    const int variant = phase / 2;
    if (phase % 2 == 1)
    {
        if (variant == 0) settings.lightCacheReach = 0;
        if (variant == 1) settings.lightBounces = false;
        if (variant == 2) settings.lightHitLight = false;
    }
    return settings;
}

void drawLightCache()
{
    const fxui::Item item({.id = "lightCache", .name = "Light cache", .summary = "How distant a hit reads the light cache, how fast its cells follow a change, and what the light's rays cost.",
        .keywords = "cache lag light global illumination distance samples compare cost bounce"});
    if (!item) return;

    performance::watch();
    auto options = core::currentDiagnostics();
    bool changed = false;
    changed |= fxui::slider("Hits from the cache past (cells)", options.lightCacheReach,
        {.min = 0.0f, .max = 32.0f, .standard = 4.0f, .format = "%.0f",
            .tip = "A light ray's first hit this many of its own cells away or further takes its light from the cache, about four rays "
                   "fewer. Fewer cells: more hits, more cost saved, more blur and lag. 0 turns it off."});
    changed |= fxui::slider("Cache samples a cell", options.hashMaxSamples,
        {.min = 1.0f, .max = 32.0f, .standard = 16.0f, .format = "%.0f",
            .tip = "Cells average over this many samples: fewer follow a light's change sooner, with more noise."});
    if (changed && !ablation.running) core::applyDiagnostics(options);

    const auto costs = performance::status().costs;
    const auto rays = costs.passes[static_cast<std::size_t>(performance::Pass::GiRays)];
    fxui::note(Tone::Dim, std::format("Now: GPU frame {:.2f} ms, the global illumination's rays {:.2f} ms.", costs.gpuFrame, rays));

    if (!ablation.running)
    {
        if (fxui::button("Compare what the rays cost (36 s)",
                "Times the rays with everything on, then with the light cache off, their bounce rays out, and the hits' own light out, six "
                "seconds each. Stand still, in the light you want to judge."))
        {
            ablation = {};
            ablation.running = true;
            ablation.base = options;
            ablation.phaseStart = GetTickCount64();
            core::applyDiagnostics(ablatedSettings(0));
        }
    }
    else
    {
        const auto now = GetTickCount64();
        const int variant = ablation.phase / 2;
        if (now - ablation.phaseStart >= LightAblation::SettleMs)
        {
            if (ablation.phase % 2 == 1)
            {
                ablation.frame[variant] += costs.gpuFrame;
                ablation.rays[variant] += rays;
                ablation.count[variant] += 1;
            }
            else
            {
                ablation.baseFrame += costs.gpuFrame;
                ablation.baseRays += rays;
                ablation.baseCount += 1;
            }
        }
        fxui::note(Tone::Warn, std::format("Comparing: step {} of {}, {}{}. Stay on this page and keep still.", ablation.phase + 1,
            LightAblation::Phases, ablation.phase % 2 ? "without " : "with ", ablation.phase % 2 ? LightAblation::Names[variant] : "everything"));
        if (now - ablation.phaseStart >= LightAblation::PhaseMs)
        {
            ablation.phase += 1;
            ablation.phaseStart = now;
            const bool done = ablation.phase >= LightAblation::Phases;
            core::applyDiagnostics(done ? ablation.base : ablatedSettings(ablation.phase));
            if (done)
            {
                ablation.running = false;
                const auto mean = [](double sum, double count) { return count > 0 ? sum / count : 0.0; };
                const double baseFrame = mean(ablation.baseFrame, ablation.baseCount), baseRays = mean(ablation.baseRays, ablation.baseCount);
                std::string text = std::format("Everything on: GPU frame {:.2f} ms, rays {:.2f} ms.", baseFrame, baseRays);
                for (int i = 0; i < LightAblation::Variants; ++i)
                {
                    const double frame = mean(ablation.frame[i], ablation.count[i]), withoutRays = mean(ablation.rays[i], ablation.count[i]);
                    text += std::format("  Without {}: {:.2f} and {:.2f} ms, so it costs {:.2f} of the frame and {:.2f} of the rays.",
                        LightAblation::Names[i], frame, withoutRays, baseFrame - frame, baseRays - withoutRays);
                }
                ablation.result = text;
                log(text);
            }
        }
    }
    if (!ablation.result.empty()) fxui::note(Tone::Ok, ablation.result);
}
// What frames cost, on file.
void drawCostRecording()
{
    const fxui::Item item({.id = "costRecording", .name = "Costs on file", .summary = "A row a second of what frames cost, to read after playing.",
        .keywords = "performance recorder record frame time gpu log"});
    if (!item) return;

    bool isOn = perf_record::isRecording();
    if (fxui::checkbox("Record the costs", isOn,
            "Writes every pass, the rays, the lights, the map and the clock to DevKit/perf once a second, until turned off, across starts. "
            "tools/automation/perf.py log reads it. Debug cases keep their own costs either way."))
        perf_record::setRecording(isOn);
}

}

void install(const std::filesystem::path& addonDirectory) noexcept
{
    devkit::setName("PhantomFX");
    devkit::setToast([](devkit::Tone tone, std::string_view text, std::string_view detail) { fxui::toast(toneOf(tone), text, detail); });
    devkit::setLogger(&log);
    perf_record::install(addonDirectory / "DevKit" / "perf");
    devkit_link::install();
}

/** The game thread, at the main loop's frame start: each tool that runs there, in order. */
void onFrameStart() noexcept
{
    devkit::travel::onFrameStart();
    devkit::cutscenes::onFrameStart();
    devkit::game_lua::onFrameStart();
    devkit_link::onFrameStart();
    perf_record::onFrameStart();
    photo_mode::onFrameStart();
}

std::filesystem::path recordingDirectory() noexcept
{
    return devkit_link::recordingDirectory();
}

void drawPage()
{
    const fxui::Page page(Section::Developer);
    if (!page) return;

    if constexpr (ToolsOnly) fxui::note(Tone::Dim, "The tools build: PhantomFX's own changes to the frame are off; its tools here read what of it runs.");
    else
    {
        drawCostRecording();
        remaster::drawTool();
        drawLightCache();
    }
    if constexpr (DiagnosticLog && !ToolsOnly)
    {
        auto options = core::currentDiagnostics();
        const fxui::Item item({.id = "globalIlluminationLog", .name = "Global illumination log", .keywords = "log often light fade"});
        if (item
            && fxui::checkbox("Log global illumination often", options.logGlobalIlluminationOften,
                "With global illumination on, its light and each step's largest value go to the log every few frames, not every ten seconds: to see "
                "how fast light fades from each step after a light goes away."))
            core::applyDiagnostics(options);
    }
}
}
