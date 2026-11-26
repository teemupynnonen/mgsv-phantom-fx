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

// The light cache's A/B: the cache off and on in turn, each for a few seconds, with the GPU's cost averaged over what is
// left after the settling. Run from the Light cache item, so it needs the Developer page open.
struct CacheCompare
{
    static constexpr int Phases = 8; // off, on, off, on, ...
    static constexpr std::uint64_t PhaseMs = 6000, SettleMs = 2000;
    bool running{};
    int phase{};
    std::uint64_t phaseStart{};
    float savedReach{10};
    double sumFrame[2]{}, sumRays[2]{}, count[2]{};
    std::string result;
};

CacheCompare compare;

void drawLightCache()
{
    const fxui::Item item({.id = "lightCache", .name = "Light cache", .summary = "How distant a hit reads the light cache, and how fast its cells follow a change.",
        .keywords = "cache lag light global illumination distance samples compare cost"});
    if (!item) return;

    performance::watch();
    auto options = core::currentDiagnostics();
    bool changed = false;
    changed |= fxui::slider("Hits from the cache past (m)", options.lightCacheReach,
        {.min = 0.0f, .max = 50.0f, .standard = 10.0f, .format = "%.0f",
            .tip = "A light ray's first hit this far or further takes its light from the cache, about four rays fewer. 0 turns it off."});
    changed |= fxui::slider("Cache samples a cell", options.hashMaxSamples,
        {.min = 1.0f, .max = 32.0f, .standard = 16.0f, .format = "%.0f",
            .tip = "Cells average over this many samples: fewer follow a light's change sooner, with more noise."});
    if (changed && !compare.running) core::applyDiagnostics(options);

    const auto costs = performance::status().costs;
    const auto rays = costs.passes[static_cast<std::size_t>(performance::Pass::GiRays)];
    fxui::note(Tone::Dim, std::format("Now: GPU frame {:.2f} ms, the global illumination's rays {:.2f} ms.", costs.gpuFrame, rays));

    if (!compare.running)
    {
        if (fxui::button("Compare the cache off and on (48 s)",
                "Switches the cache off and on four times, six seconds each, and averages what the GPU frame and the rays cost. Stand still, in the light you want to judge."))
        {
            compare = {};
            compare.running = true;
            compare.savedReach = options.lightCacheReach > 0 ? options.lightCacheReach : 10.0f;
            compare.phaseStart = GetTickCount64();
            options.lightCacheReach = 0;
            core::applyDiagnostics(options);
        }
    }
    else
    {
        const auto now = GetTickCount64();
        const int which = compare.phase % 2; // 0 off, 1 on
        if (now - compare.phaseStart >= CacheCompare::SettleMs)
        {
            compare.sumFrame[which] += costs.gpuFrame;
            compare.sumRays[which] += rays;
            compare.count[which] += 1;
        }
        fxui::note(Tone::Warn, std::format("Comparing: step {} of {}, the cache {}. Stay on this page and keep still.", compare.phase + 1,
            CacheCompare::Phases, which ? "on" : "off"));
        if (now - compare.phaseStart >= CacheCompare::PhaseMs)
        {
            compare.phase += 1;
            compare.phaseStart = now;
            const bool done = compare.phase >= CacheCompare::Phases;
            options.lightCacheReach = done || compare.phase % 2 == 1 ? compare.savedReach : 0.0f;
            core::applyDiagnostics(options);
            if (done)
            {
                compare.running = false;
                const auto mean = [&](const double* sum, int i) { return compare.count[i] > 0 ? sum[i] / compare.count[i] : 0.0; };
                compare.result = std::format("Cache off: GPU frame {:.2f} ms, rays {:.2f} ms.  On from {:.0f} m: GPU frame {:.2f} ms, rays {:.2f} ms.  "
                                             "The cache saves {:.2f} ms of the frame, {:.2f} ms of the rays.",
                    mean(compare.sumFrame, 0), mean(compare.sumRays, 0), compare.savedReach, mean(compare.sumFrame, 1), mean(compare.sumRays, 1),
                    mean(compare.sumFrame, 0) - mean(compare.sumFrame, 1), mean(compare.sumRays, 0) - mean(compare.sumRays, 1));
                log(compare.result);
            }
        }
    }
    if (!compare.result.empty()) fxui::note(Tone::Ok, compare.result);
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
