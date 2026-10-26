// The Phantom FX settings window in ReShade's overlay: fxui's frame budget window with Phantom FX's
// sections and settings. The costs and states come from menu_budget.cpp, the budget page from
// performance_menu.cpp.
#include "character.hpp"
#include "dev_tools.hpp"
#include "draw_distance.hpp"
#include "frame_generation.hpp"
#include "frame_log.hpp"
#include "fsr_frame_generation.hpp"
#include "fxui/fxui.hpp"
#include "game_lua.hpp"
#include "global_illumination.hpp"
#include "hdr.hpp"
#include "log.hpp"
#include "menu_budget.hpp"
#include "performance.hpp"
#include "presenter.hpp"
#include "travel.hpp"
#include "upscaler.hpp"
#include "weather.hpp"

#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace phantom_fx::upscaler
{
namespace
{
using fxui::Choice;
using fxui::State;
using fxui::Tone;
using menu::Section;

constexpr Settings Defaults{};

// The upscaler's one line on what it is doing right now.
void drawStatus(const Status& state, const Settings& wanted)
{
    const auto& name = state.upscalerName;
    const auto nativeMode = label(Mode::Dlaa, wanted.upscaler);
    // FSR has no presets.
    const auto preset = wanted.upscaler == Upscaler::Dlss ? std::format(", preset {}", label(wanted.preset)) : std::string();
    if (!state.supportedBuild) fxui::statusLine(Tone::Bad, "This game version is not supported, so upscaling stays off.");
    else if (wanted.mode == Mode::Off) fxui::statusLine(Tone::Dim, "Upscaling is off. The game's FXAA is in use.");
    else if (state.failed) fxui::statusLine(Tone::Bad, std::format("{} stopped: {} Choose a mode or upscaler again to retry.", name, state.failure));
    else if (upscales(wanted.mode) && !state.upscalingAvailable)
        fxui::statusLine(Tone::Bad, std::format("Upscaling is unavailable (see PhantomFX.log); {} still works.", nativeMode));
    else if (currentDiagnostics().gameFxaa) fxui::statusLine(Tone::Warn, "Showing the game's FXAA for comparison (Diagnostics).");
    else if (!state.fxaaRecognised) fxui::statusLine(Tone::Warn, "Waiting for the game to draw its anti-aliasing.");
    else if (!state.running && state.lastFallback.empty()) fxui::statusLine(Tone::Warn, std::format("{} is waiting for the game's 3D view.", name));
    else if (!state.running) fxui::statusLine(Tone::Warn, std::format("{} is waiting: {}.", name, state.lastFallback));
    else if (state.renderWidth == state.outputWidth && state.renderHeight == state.outputHeight)
        fxui::statusLine(Tone::Ok, std::format("{} {} active at {}\xC3\x97{}{}.", name, nativeMode, state.outputWidth, state.outputHeight, preset));
    else
        fxui::statusLine(Tone::Ok, std::format("{} {} active: renders {}\xC3\x97{}, outputs {}\xC3\x97{}{}.", name, label(wanted.mode, wanted.upscaler),
            state.renderWidth, state.renderHeight, state.outputWidth, state.outputHeight, preset));
}

void drawFrameGeneration(Settings& wanted)
{
    // The route the game started with is the one that runs until a restart.
    const bool fsrRoute = presenter::route() == presenter::Route::Fsr;
    const bool sameRoute = (wanted.frameGeneration == FrameGeneration::Fsr) == fsrRoute;
    const bool on = wanted.frameGeneration != FrameGeneration::Off;
    State state = State::Off;
    std::string message;
    if (presenter::active() && on && !sameRoute)
    {
        state = State::Restart;
        message = "Restart the game to switch frame generation.";
    }
    else if (presenter::active())
    {
        state = on ? State::Working : State::Off;
        message = fsrRoute ? fsr_frame_generation::status() : frame_generation::status();
    }
    else if (on)
    {
        // Started with the game but not running: the presenter says why. Otherwise turned on since.
        const auto reason = presenter::status();
        const bool tried = presenter::installed() && sameRoute && !reason.empty();
        state = tried ? State::NotRunning : State::Restart;
        message = tried ? reason : "Restart the game to start frame generation.";
    }

    const fxui::Item item({.id = "frameGeneration",
        .name = "Frame generation",
        .tip = "Frames generated between the ones the game renders, with either upscaler. DLSS Frame Generation: 2x needs an RTX 40, 3x to 6x "
               "and Dynamic an RTX 50. Dynamic reaches the display's refresh rate. It turns on NVIDIA Reflex. FSR frame generation: 2x on any "
               "GPU, machine-learned on Radeon RX 9000. Turning it on, or switching between DLSS and FSR, takes a restart, as the game then "
               "presents through Direct3D 12; other changes apply at once.",
        .summary = "Frames generated between the rendered ones. Turning it on, or switching between DLSS and FSR, takes a restart.",
        .keywords = "DLSS FSR Reflex 2x 3x 4x 5x 6x dynamic",
        .state = state,
        .message = message});
    if (!item) return;

    // Until Streamline reports what the GPU supports, every DLSS mode is offered.
    const auto supported = frame_generation::capabilities();
    std::vector<Choice<FrameGeneration>> modes;
    for (int i = 0; i < static_cast<int>(FrameGeneration::Count); ++i)
    {
        const auto mode = static_cast<FrameGeneration>(i);
        bool unsupported{};
        if (mode == FrameGeneration::Dynamic) unsupported = supported.maxMultiplier && !supported.dynamic;
        else if (multiplier(mode)) unsupported = supported.maxMultiplier && multiplier(mode) > supported.maxMultiplier;
        modes.push_back({mode, label(mode).data(), nullptr, !unsupported, unsupported ? "This GPU does not support it." : nullptr});
    }
    fxui::combo("Frame generation", wanted.frameGeneration, std::span<const Choice<FrameGeneration>>(modes));
}

void drawUpscaling(const Status& state, Settings& wanted)
{
    const fxui::Page page(Section::Upscaling, "Anti-aliasing and upscaling in place of the game's FXAA, and frames generated between the rendered ones.");
    if (!page) return;

    drawStatus(state, wanted);
    if (const fxui::Item item({.id = "upscaler",
            .tip = "NVIDIA DLSS needs an NVIDIA RTX GPU. AMD FSR runs on any GPU: FSR 4 on Radeon RX 7000 and 9000, FSR 3.1 on the rest.",
            .summary = "Replaces the game's FXAA, at full resolution or upscaled from a lower one. The HUD stays at full resolution.",
            .keywords = "DLSS FSR DLAA native AA mode quality balanced performance preset sharpening texture detail bias render scale"});
        item)
    {
        constexpr Choice<Upscaler> upscalers[]{{Upscaler::Dlss, "DLSS"}, {Upscaler::Fsr, "FSR"}};
        fxui::segmented("Upscaler", wanted.upscaler, upscalers);

        std::vector<Choice<Mode>> modes;
        for (const auto mode : {Mode::Off, Mode::Dlaa, Mode::Quality, Mode::Balanced, Mode::Performance, Mode::UltraPerformance, Mode::Custom})
            modes.push_back({mode, label(mode, wanted.upscaler).data()});
        fxui::combo("Mode", wanted.mode, std::span<const Choice<Mode>>(modes),
            "DLAA and Native AA replace the game's FXAA at full resolution. Quality to Ultra Performance render the 3D scene below the output "
            "resolution and upscale it; the HUD and menus stay at full resolution. Off leaves the game unchanged.");
        if (wanted.mode == Mode::Custom)
            fxui::slider("Render scale", wanted.customScale,
                {.min = Settings::MinScale, .max = Settings::MaxScale, .standard = Defaults.customScale, .format = "%.0f%%", .shown = 100, .onRelease = true,
                    .tip = "Render width as a share of the output width. Applies when released."});

        ImGui::BeginDisabled(wanted.mode == Mode::Off);
        if (wanted.upscaler == Upscaler::Dlss)
        {
            const Choice<Preset> presets[]{{Preset::Default, label(Preset::Default).data()}, {Preset::K, label(Preset::K).data()},
                {Preset::J, label(Preset::J).data()}, {Preset::M, label(Preset::M).data()}, {Preset::L, label(Preset::L).data()}};
            fxui::combo("DLSS preset", wanted.preset, presets, "Default lets DLSS choose (K for DLAA).");
        }
        fxui::slider("Sharpening", wanted.sharpness,
            {.max = Settings::MaxSharpness, .standard = Defaults.sharpness, .format = "%.0f%%", .shown = 100, .tip = "AMD FidelityFX RCAS on the upscaled image. 0% is off."});
        fxui::slider("Texture detail bias", wanted.mipBiasOffset,
            {.min = Settings::MinMipBias, .max = Settings::MaxMipBias, .standard = Defaults.mipBiasOffset, .format = "%+.2f", .onRelease = true,
                .tip = "Added to the automatic texture LOD bias for the render size. Lower is sharper but can shimmer. Applies when released."});
        fxui::hint("Lower is sharper but can shimmer. Applies when released.");
        ImGui::EndDisabled();
    }
    drawFrameGeneration(wanted);
}

void sunSize(Settings& wanted)
{
    fxui::slider("Sun size", wanted.sunSize,
        {.min = Settings::MinSunSize, .max = Settings::MaxSunSize, .standard = Defaults.sunSize, .format = "%.2f\xC2\xB0",
            .tip = "The sun's angular diameter, which sets how fast shadows soften with distance. It also sets the sun for the global illumination. 0.53 is the real sun."});
}

void drawShadows(Settings& wanted)
{
    const fxui::Page page(Section::Shadows, "Shadows of the sun and of lamps. The ray-traced ones replace the screen-space ones while on.");
    if (!page) return;

    if (const fxui::Item item({.id = "rayTracedShadows",
            .tag = "Experimental",
            .tip = "The sun's shadows from rays traced through the game's own scene: as soft as the sun's disc makes them, exact where objects meet "
                   "the ground, and as far as you can see, where the game's shadows end at about 90 m. It replaces the game's shadows, soft shadows "
                   "and sun contact shadows while on. Needs a GPU that traces rays (DXR 1.1). Off keeps the game's shadow map.",
            .summary = "Sun shadows traced through the game's own scene: soft, exact where things touch, and as far as you can see.",
            .keywords = "RT DXR sun size vanilla"});
        item)
    {
        sunSize(wanted);
        fxui::checkbox("Show the game's shadows too, near you", wanted.vanillaShadowsWithRayTracing,
            "The game's own shadow map as well, near you (about 90 m): the darker of its shadow and the rays' shows. Off, the rays alone shadow the sun.");
    }

    if (const fxui::Item item({.id = "softShadows",
            .tip = "Contact-hardening soft shadows from the sun's shadow map: sharp where an object meets the ground, softer the further the shadow "
                   "falls from it, as the sun's disc gives. Off keeps the game's evenly sharp shadow edges.",
            .summary = "Sharp where an object meets the ground, softer the further the shadow falls.",
            .keywords = "sun size"});
        item)
        sunSize(wanted);

    fxui::item({.id = "sunContactShadows",
        .tip = "Bend Studio's screen-space shadows for the sun: short shadows traced through the depth buffer. They add the contact shadows the "
               "sun's shadow map misses (feet, rocks, folds of clothing). Cheap: one trace a frame.",
        .summary = "The short shadows the shadow map misses: feet, rocks, folds of clothing.",
        .keywords = "screen-space SSS"});

    if (const fxui::Item item({.id = "rayTracedLights",
            .tag = "Experimental",
            .tip = "Every lamp, spot light and flashlight casts shadows from rays traced through the game's own scene, in place of the game's "
                   "shadow maps or none at all: most of the game's lights, the flashlights among them, light through walls and characters. Lights "
                   "without specular get the game's own. With global illumination on, their light bounces too. Needs a GPU that traces rays (DXR 1.1).",
            .summary = "Every lamp, spot light and flashlight casts traced shadows, where most of the game's lights shine through walls.",
            .keywords = "RT DXR ray quality flashlight brightness"});
        item)
    {
        constexpr Choice<int> qualities[]{{1, "Low 1"}, {2, "Medium 2"}, {4, "High 4"}, {8, "Ultra 8"}};
        fxui::segmented("Ray quality", wanted.rayQuality, qualities,
            "Shadow rays a pixel for the lights' soft shadows, shared over the lights that reach it. More rays leave less noise for the filter to "
            "smooth; the cost grows with them where lamps light the screen.");
        fxui::slider("Flashlight brightness", wanted.flashlightBrightness,
            {.min = Settings::MinFlashlightBrightness, .max = Settings::MaxFlashlightBrightness, .standard = Defaults.flashlightBrightness, .format = "%.1fx",
                .logarithmic = true,
                .tip = "The weapon flashlights, yours and the soldiers', brighter than the game's; they reach further with it, as a brighter light "
                       "fades to the same dimness further away. 1x is the game's: about 5 times the moonlight at 10 m and cut off at 30 m. 8x is "
                       "about a real 1980s weapon light against the game's night. Only the look: the guards see as before."});
    }

    fxui::item({.id = "lampContactShadows",
        .tip = "Bend Studio's screen-space shadows for every lamp, spot light and flashlight, which gives lights that have no shadow of their own "
               "one. Costly: one trace per light, so the frame rate drops where many lights are on screen, as in a base at night.",
        .summary = "Contact shadows for every lamp and flashlight. One trace per light, so it costs more where many lights are on screen.",
        .keywords = "screen-space SSS"});
}

void drawLighting(Settings& wanted)
{
    const fxui::Page page(Section::Lighting,
        "Light that bounces between surfaces, and the shade where they meet. Global illumination replaces both screen-space effects while on.");
    if (!page) return;

    if (const fxui::Item item({.id = "globalIllumination",
            .tag = "Experimental",
            .tip = "Ray-traced indirect light: rays for every 2 x 2 pixels, each hit lit afresh by the sun and the lights, its bounce from a "
                   "light cache, then NVIDIA's NRD denoiser. Sunlight, skylight and lamplight bounce between surfaces, also those off screen, and "
                   "the sky only lights what it can reach. It replaces the game's baked probe light and SSAO, and GTVBAO and SSILVB while on. Needs "
                   "a GPU that traces rays (DXR 1.1). Off keeps the game's indirect light.",
            .summary = "Sun and sky light bounce between surfaces, also those off screen, in place of the game's baked light.",
            .keywords = "RT DXR GI strength ray quality sun size"});
        item)
    {
        constexpr Choice<int> qualities[]{{1, "Low 1"}, {2, "Medium 2"}, {4, "High 4"}};
        fxui::segmented("Ray quality", wanted.globalIlluminationRays, qualities,
            "Rays for every 2 x 2 pixels a frame, twice on grass and leaves. More rays leave less of the noise that crawls over shaded surfaces "
            "(High has half of Low's); the rays' cost grows with them.");
        fxui::slider("Strength", wanted.globalIlluminationStrength,
            {.max = Settings::MaxGlobalIlluminationStrength, .standard = Defaults.globalIlluminationStrength,
                .tip = "1 is physically based; higher brightens the indirect light, 0 leaves only the sun and the lamps."});
        sunSize(wanted);
    }

    if (const fxui::Item item({.id = "ambientOcclusion",
            .tip = "Ground-truth visibility bitmask ambient occlusion in place of the game's SSAO, whatever its SSAO setting. It darkens only the "
                   "indirect light, so sunlit surfaces keep their brightness. Off keeps the game's SSAO, which darkens all light.",
            .summary = "Shade where surfaces meet, on the indirect light only, so sunlit surfaces keep their brightness.",
            .keywords = "AO SSAO radius strength"});
        item)
    {
        fxui::slider("Radius", wanted.occlusionRadius,
            {.min = Settings::MinOcclusionRadius, .max = Settings::MaxOcclusionRadius, .standard = Defaults.occlusionRadius, .format = "%.2f m",
                .tip = "How far around a surface GTVBAO looks for what occludes it."});
        fxui::slider("Strength", wanted.occlusionStrength,
            {.max = Settings::MaxOcclusionStrength, .standard = Defaults.occlusionStrength,
                .tip = "1 is physically based; higher darkens occluded areas more, 0 turns the occlusion off."});
    }

    if (const fxui::Item item({.id = "indirectLight",
            .tip = "Screen-space indirect lighting with visibility bitmask: light bounces off nearby surfaces onto others, taking their colour, such "
                   "as sunlit ground brightening the underside of a jeep. It only knows what is on screen. Off leaves the game's indirect light unchanged.",
            .summary = "Light bounces off nearby surfaces and takes their colour. It only knows what is on screen.",
            .keywords = "SSGI bounce radius strength"});
        item)
    {
        fxui::slider("Radius", wanted.indirectLightRadius,
            {.min = Settings::MinIndirectLightRadius, .max = Settings::MaxIndirectLightRadius, .standard = Defaults.indirectLightRadius, .format = "%.2f m",
                .tip = "How far around a surface SSILVB looks for surfaces that light it."});
        fxui::slider("Strength", wanted.indirectLightStrength,
            {.max = Settings::MaxIndirectLightStrength, .standard = Defaults.indirectLightStrength,
                .tip = "1 is physically based; higher brightens the bounced light, 0 turns it off."});
    }
}

void drawAtmosphere(Settings& wanted)
{
    const fxui::Page page(Section::Atmosphere, "Fog with the sun shadowed in it, and the sky.");
    if (!page) return;

    const fxui::Item item({.id = "volumetricFog",
        .tip = "Fog with the sun shadowed in it: light shafts through trees, buildings and dust, and shade where the sun cannot reach. Shadows reach "
               "about 90 m, as the game's do. Off leaves the game's fog unchanged.",
        .summary = "Light shafts through trees, buildings and dust, and shade where the sun cannot reach.",
        .keywords = "sky look physical MGSV haze light shafts"});
    if (!item) return;

    constexpr Choice<AtmosphereLook> looks[]{{AtmosphereLook::Mgsv, "MGSV"}, {AtmosphereLook::Physical, "Physical"}};
    fxui::segmented("Look", wanted.atmosphereLook, looks,
        "MGSV keeps the game's own sky and haze colours, with the sun shadowed in the fog. Physical draws the sky and the air as they are in nature "
        "(Hillaire's atmosphere, as in Unreal Engine), as bright as the game's sky, with height fog that gathers low, as thick as the game's weather "
        "makes its fog.");
    if (wanted.atmosphereLook == AtmosphereLook::Physical)
        fxui::slider("Haze", wanted.atmosphereHaze,
            {.standard = Defaults.atmosphereHaze, .format = "%.0f%%", .shown = 100,
                .tip = "Dust in the air that fades the distance. A matter of taste: no setting is more physically based or closer to the game's look. 0% "
                       "(the default) is clear air, about 120 km of view on a clear day; 100% is about 8 km, with a paler sky. Fog, sandstorms and rain "
                       "are always at full thickness."});
}

void drawCamera(Settings& wanted)
{
    const fxui::Page page(Section::Camera, "The lens of photo mode.");
    if (!page) return;

    if (const fxui::Item item({.id = "photoMode",
            .name = "Photo mode",
            .summary = "F2 opens it. Fly with WASD and the mouse, Space and Ctrl for up and down, Shift for faster. F3 hides the HUD, F4 the blur.",
            .keywords = "f-stop aperture blur camera depth of field DOF"});
        item)
        fxui::slider("f-stop", wanted.fNumber,
            {.min = Settings::MinFNumber, .max = Settings::MaxFNumber, .standard = Defaults.fNumber, .format = "f/%.1f", .logarithmic = true,
                .tip = "Photo mode's aperture, for the game's depth of field: a lower f-stop blurs more of what is out of focus. It needs Depth of "
                       "Field on in the game's graphics settings. F4 turns the blur off."});
}

void drawDisplay(Settings& wanted)
{
    const fxui::Page page(Section::Display, "HDR output, and the frame rate cap.");
    if (!page) return;

    if (const fxui::Item item({.id = "hdr",
            .tip = "The game in HDR10 on an HDR display: the sun, lamps, fire and reflections shine brighter than the game's white. It needs HDR on "
                   "for the display in Windows. Turning it on or off takes a restart; the brightness applies at once.",
            .summary = "The sun, lamps and fire shine brighter than the game's white. Needs HDR on for the display in Windows.",
            .keywords = "peak brightness paper white nits"});
        item)
    {
        // Until moved, the slider shows the display's own peak, which a reset goes back to.
        const float display = std::clamp(hdr::peakNitsFor(0), Settings::MinPeakNits, Settings::MaxPeakNits);
        float peak = std::clamp(hdr::peakNitsFor(wanted.peakNits), Settings::MinPeakNits, Settings::MaxPeakNits);
        if (fxui::slider("Peak brightness", peak,
                {.min = Settings::MinPeakNits, .max = Settings::MaxPeakNits, .standard = display, .format = "%.0f nits", .logarithmic = true,
                    .tip = "The brightest your display shows; by default, what it reports. The brightest the game gets lands on it."}))
            wanted.peakNits = peak == display ? 0 : peak;
        if (!wanted.peakNits) fxui::hint("What this display reports.");
        fxui::slider("Paper white", wanted.paperWhiteNits,
            {.min = Settings::MinPaperWhiteNits, .max = Settings::MaxPaperWhiteNits, .standard = Defaults.paperWhiteNits, .format = "%.0f nits",
                .tip = "How bright the game's white is, the HUD and menus included. 203 nits is the HDR reference; raise it in a bright room."});
    }

    fxui::item({.id = "unlockFrameRate",
        .name = "Unlock frame rate",
        .tip = "Lifts the game's 60 fps cap, like framerate_control \"Variable\" in its settings file. Off keeps the game's setting.",
        .summary = "Lifts the game's 60 fps cap.",
        .keywords = "fps 60 cap limit",
        .toggle = &wanted.unlockFps,
        .state = wanted.unlockFps ? State::Working : State::Off});
}

// "06:30" for a time in hours.
std::string clockText(float hours)
{
    const auto minutes = std::min(24 * 60 - 1, static_cast<int>(hours * 60));

    return std::format("{:02}:{:02}", minutes / 60, minutes % 60);
}

// The game's weather and time of day, to see the sky in every condition.
void drawWeather()
{
    const auto now = weather::state();
    std::optional<State> state;
    std::string message;
    if (!weather::supported())
    {
        state = State::Unavailable;
        message = "Unavailable for this game version.";
    }
    else if (!now.available) message = "Available in a mission or free roam.";
    else if (now.requested && !now.forced) message = std::format("The game's weather: {}.", weather::label(*now.requested));
    else if (now.requested && now.requested != now.forced)
    {
        state = State::NotRunning;
        message = std::format("The game overrides it with {} for now.", weather::label(*now.requested));
    }

    const fxui::Item item({.id = "weather",
        .name = "Weather and time",
        .summary = "The game's clock and weather, to see the sky in every condition.",
        .keywords = "time of day clock sunny cloudy rainy sandstorm foggy fog density",
        .state = state,
        .message = message});
    if (!item || !weather::supported() || !now.available) return;

    float hours = now.clock / 3600;
    if (fxui::slider("Time of day", hours, {.max = 24, .text = clockText, .tip = "Sets the game's clock while dragged. The day goes on from there unless the clock is stopped."}))
        weather::setClock(hours * 3600);

    bool stopped = now.clockStopped;
    if (fxui::checkbox("Stop the clock", stopped)) weather::stopClock(stopped);

    static float fogDensity = 0.1f;
    const auto shown = now.forced ? std::string(weather::label(*now.forced)) : std::string("The game's");
    if (fxui::beginCombo("Weather", shown.c_str(),
            "Switches the weather at once and holds it over the game's own, as its mission scripts do. Greyed out: this location has no settings for "
            "that weather. A checkpoint saved meanwhile keeps it; The game's gives it back."))
    {
        if (fxui::option("The game's", !now.forced)) weather::release();
        for (std::size_t i = 0; i < now.typeAvailable.size(); ++i)
        {
            const auto type = static_cast<weather::Type>(i);
            if (fxui::option(std::string(weather::label(type)).c_str(), now.forced == type, now.typeAvailable[i], "This location has no settings for it."))
                weather::force(type, fogDensity);
        }
        fxui::endCombo();
    }

    if (now.forced == weather::Type::Foggy && fxui::slider("Fog density", fogDensity, {.standard = 0.1f})) weather::force(weather::Type::Foggy, fogDensity);
}

// Play as another of the game's characters.
void drawCharacter()
{
    const auto now = character::state();
    std::optional<State> state;
    std::string message;
    if (!character::supported())
    {
        state = State::Unavailable;
        message = "Unavailable for this game version.";
    }
    else if (!now.available) message = "Available in a mission or free roam.";

    const fxui::Item item({.id = "character",
        .name = "Character",
        .summary = "Play as another of the game's characters, in any outfit. A checkpoint keeps it.",
        .keywords = "Snake avatar soldier Ocelot Quiet outfit face",
        .state = state,
        .message = message});
    if (!item || !character::supported() || !now.available) return;

    const auto characters = character::characters();
    std::vector<std::string_view> names;
    for (const auto& one : characters) names.push_back(one.name);
    const auto current = std::ranges::find_if(characters, [&](const character::Character& c) { return c.type == now.type && c.parts == now.parts; });
    const auto chosen = current != characters.end() ? std::optional(static_cast<std::size_t>(current - characters.begin())) : std::nullopt;
    const auto shown = chosen ? std::string(current->name) : std::string("Another outfit");
    if (const auto picked = fxui::searchCombo("Character", shown.c_str(), names, chosen,
            "Switches the player's character and outfit at once, as the game's scripts do for cutscenes. A checkpoint keeps it; the next mission starts "
            "with the character chosen for its sortie."))
        character::choose(characters[*picked]);

    const auto faces = character::faces(now.type);
    if (faces.empty()) return;

    std::vector<std::string_view> faceNames;
    for (const auto& face : faces) faceNames.push_back(face.name);
    const auto face = std::ranges::find(faces, now.face, &character::Face::id);
    const auto faceChosen = face != faces.end() ? std::optional(static_cast<std::size_t>(face - faces.begin())) : std::nullopt;
    const auto faceShown = faceChosen ? face->name : std::format("Face {}", now.face);
    if (const auto picked = fxui::searchCombo("Face", faceShown.c_str(), faceNames, faceChosen,
            "Any face of the game's soldiers. Search by number, skin (light, olive, fair, brown, dark), headgear or tattoo."))
        character::chooseFace(faces[*picked].id);
}

void drawWorld(Settings& wanted)
{
    const fxui::Page page(Section::World, "How far the world is drawn, its weather and time, and who you play.");
    if (!page) return;

    const auto problem = draw_distance::problem();
    if (const fxui::Item item({.id = "drawDistance",
            .name = "Draw distance",
            .tip = "How far the world is drawn. Vanilla leaves the game's own settings. Extended loads full detail (props, bushes, trees, rocks) out to "
                   "about 450 m instead of 320 m, and the game's coarser copies of the world out to about 900 m and 1.8 km instead of 640 m and 1.3 km. "
                   "It also keeps more detail at a distance, upscaled or not, and draws models and vegetation farther, whatever Model Detail is. Far "
                   "loads the coarser copies out to about 1.2 km and 2.3 km. Loading farther takes effect when a location loads, as on Continue from "
                   "the title screen. It costs memory and CPU time.",
            .summary = "Full detail and the distant world load farther. It costs memory and CPU time, not GPU time.",
            .keywords = "LOD vanilla extended far",
            .state = problem.empty() ? std::nullopt : std::optional(State::Unavailable),
            .message = problem});
        item)
    {
        constexpr Choice<DrawDistance> distances[]{{DrawDistance::Vanilla, "Vanilla"}, {DrawDistance::Extended, "Extended"}, {DrawDistance::Far, "Far"}};
        fxui::segmented("Draw distance", wanted.drawDistance, distances);
        if (draw_distance::waitsForLocation()) fxui::note(Tone::Dim, "Loading farther or nearer takes effect when a location loads, as on Continue.");
    }

    drawWeather();
    drawCharacter();
}

void drawDiagnostics(const Status& state)
{
    const fxui::Page page(Section::Diagnostics, "For troubleshooting. Leave these at their defaults.");
    if (!page) return;

    auto options = currentDiagnostics();
    bool changed{};
    if (const fxui::Item item({.id = "debugView", .name = "Debug view", .keywords = "motion vectors object motion shadow rays probe"}); item)
    {
        constexpr Choice<int> views[]{{0, "Final image"}, {1, "Motion vectors"}, {2, "Object motion (red)"}, {ShadowRaysView, "Ray-traced shadow rays"},
            {LightHistoryView, "Global illumination history"}, {ShadowStartsView, "Shadow ray starts"}};
        changed |= fxui::combo("Debug view", options.debugView, views,
            "Ray-traced shadow rays: with ray-traced shadows and an upscaler on, what blocks each pixel's ray to the sun's centre, over the image. The "
            "cross is the probe, whose findings show below (and, in a dev build, go to PhantomFX.log whenever they change).");
        if (options.debugView == LightHistoryView)
            fxui::note(Tone::Dim, "With global illumination and an upscaler on: red where the denoiser started a pixel's light over this frame or "
                                  "the last two, yellow where it holds under ten frames. Clear: a full history.");
        if (options.debugView == ShadowStartsView)
            fxui::note(Tone::Dim, "With ray-traced shadows on, where each pixel's shadow rays leave, against what the game drew. Grey: from the depth, "
                                  "the scene having no surface there. Blue: the scene's surface faces another way. Red: it lies nearer than the "
                                  "game's, cyan: farther (clear under 5 mm, full from 3 cm). Clear: on the game's surface.");
        if (options.debugView == ShadowRaysView)
            fxui::note(Tone::Dim, "Lavender: the game's models and characters. Brown: the terrain. Yellow: cut-out geometry. Olive: cut-out geometry "
                                  "without its texture, left to the shadow map. Magenta: anything else only the whole ray finds. Clear: the sun reaches.");
        if (options.debugView == ShadowRaysView || options.debugView == ShadowStartsView)
        {
            if (const auto probe = global_illumination::shadowProbe(); !probe.empty()) fxui::note(Tone::Dim, probe);
            else fxui::note(Tone::Warn, "Waiting for the ray-traced shadows (on, with an upscaler).");
        }
    }

    if (const fxui::Item item({.id = "comparisons", .name = "Comparisons", .summary = "Each effect alone, or the game's own, to compare.",
            .keywords = "jitter FXAA contact shadows GTVBAO SSILVB global illumination scene only"});
        item)
    {
        changed |= fxui::checkbox("Jitter off", options.noJitter, "The upscaler gets the same pixel positions every frame. Edges stay aliased.");
        changed |= fxui::checkbox("Game FXAA (comparison)", options.gameFxaa);
        changed |= fxui::checkbox("Sun contact shadows only", options.onlyScreenSpaceShadows,
            "With sun contact shadows on, the sun is shadowed by them alone, without the shadow map.");
        changed |= fxui::checkbox("GTVBAO only", options.showAmbientOcclusion, "With GTVBAO on, the scene shows its occlusion alone: white where nothing occludes.");
        changed |= fxui::checkbox("SSILVB only", options.showIndirectLight, "With SSILVB on, the scene shows the light it adds alone: black where nothing bounces.");
        changed |= fxui::checkbox("Global illumination only", options.showGlobalIllumination, "With global illumination on, the scene shows its indirect light alone.");
        changed |= fxui::checkbox("Global illumination scene", options.showTracedScene,
            "With global illumination on, the scene shows the geometry it traces, coloured by its normals: red where it disagrees with the game's "
            "depth, magenta where it lacks what the game drew, yellow where it has something in the sky.");
        if (const auto scene = global_illumination::status(); options.showTracedScene && !scene.empty()) fxui::note(Tone::Dim, scene);
    }
    if (changed) applyDiagnostics(options);

    if (const fxui::Item item({.id = "upscalerState", .name = "Upscaler", .keywords = "frames render output jitter fallback LOD bias"}); item)
    {
        fxui::fact("Frames", std::format("{} upscaled, {} without; {} history resets", state.evaluations, state.fallbacks, state.resets));
        fxui::fact("Render", std::format("{}\xC3\x97{}, output {}\xC3\x97{}", state.renderWidth, state.renderHeight, state.outputWidth, state.outputHeight));
        fxui::fact("Texture LOD bias", std::format("{:+.2f}", state.mipBias));
        fxui::fact("Jitter", std::format("{:.3f}, {:.3f} px", state.jitter[0], state.jitter[1]));
        if (!state.lastFallback.empty()) fxui::fact("Last fallback", state.lastFallback);
        if (frame_log::installed()) fxui::note(Tone::Dim, "F10 records the next frame into the PhantomFX-frame-log folder.");
    }
}
}

// Free roam in another of the game's maps.
void drawTravel()
{
    const bool supported = game_lua::supported();
    const fxui::Item item({.id = "travel",
        .name = "Travel",
        .summary = "Free roam in another map, after one loading screen.",
        .keywords = "teleport map location Afghanistan Africa Mother Base free roam",
        .state = supported ? std::nullopt : std::optional(State::Unavailable),
        .message = supported ? "" : "Unavailable for this game version."});
    if (!item || !supported) return;

    std::array<fxui::Option, static_cast<std::size_t>(travel::Map::Count)> maps{};
    for (std::size_t i = 0; i < maps.size(); ++i) maps[i].label = travel::name(static_cast<travel::Map>(i));
    const int pressed = fxui::segmented("Go to", -1, maps,
        "Leaves free roam as the helicopter would and arrives on foot: in Afghanistan and Africa where the game puts a player found off the map, "
        "at Mother Base on the Command platform. The game saves on the way, as it does when leaving free roam.");
    if (pressed >= 0) travel::go(static_cast<travel::Map>(pressed));
}

// Lua typed here runs in the game's own script state.
void drawLuaConsole()
{
    const bool supported = game_lua::supported();
    const fxui::Item item({.id = "luaConsole",
        .name = "Lua console",
        .summary = "Runs Lua in the game's own scripts at the next frame: an expression shows its value, print() its lines.",
        .keywords = "script execute run code vars gvars mvars svars TppMission",
        .state = supported ? std::nullopt : std::optional(State::Unavailable),
        .message = supported ? "" : "Unavailable for this game version."});
    if (!item || !supported) return;

    static std::array<char, 16 * 1024> code{};
    const bool submitted = fxui::textBox("##luaCode", code, 8);
    const auto console = game_lua::console();
    if (fxui::button("Run", "Ctrl+Enter in the box runs it too. It runs on the game thread at the next frame start, as the game's own scripts do.",
            !console.waiting)
        || submitted)
        game_lua::submit(code.data());
    if (!console.last) return;

    fxui::gap(6);
    const auto& last = *console.last;
    fxui::note(last.ran ? Tone::Ok : Tone::Bad, std::format("Run {}: {}", console.runs, last.ran ? "done" : "error"));
    if (last.text.empty()) return;

    std::string output = last.text;
    const auto lines = static_cast<float>(std::ranges::count(output, '\n') + 1);
    fxui::textBox("##luaOutput", {output.data(), output.size() + 1}, std::clamp(lines, 1.0f, 12.0f), true);
}

// Tools for testing in the game; only the dev build has this page.
void drawDeveloper()
{
    const fxui::Page page(Section::Developer, "Tools for testing in the game. Only the dev build has this page.");
    if (!page) return;

    drawTravel();
    drawLuaConsole();
    if constexpr (DiagnosticLog)
    {
        auto options = currentDiagnostics();
        const fxui::Item item({.id = "globalIlluminationLog", .name = "Global illumination log", .keywords = "log often light fade"});
        if (item
            && fxui::checkbox("Log global illumination often", options.logGlobalIlluminationOften,
                "With global illumination on, its light and each step's largest value go to the log every few frames, not every ten seconds: to see "
                "how fast light fades from each step after a light goes away."))
            applyDiagnostics(options);
    }
}

void drawMenu()
{
    auto wanted = currentSettings();
    const auto before = wanted;
    const auto state = status();
    bool enhancements = enhancementsOn();
    // The costs show in every part of the window, so its passes are timed while it is open.
    performance::watch();
    const auto budget = menu::budget(wanted, performance::status().costs);

    constexpr fxui::Key keys[]{{"F1", "Effects on/off"}, {"F2", "Photo mode"}, {"F3", "Hide the HUD"}, {"F4", "Photo blur"}};
    {
        const fxui::Window window(menu::sections(), budget,
            {.name = "Phantom FX",
                .master = &enhancements,
                .masterLabel = "Visual enhancements",
                .masterKey = "F1",
                .masterTip = "Soft shadows, contact shadows, GTVBAO, SSILVB, the volumetric fog and the ray tracing together, as set in their sections. "
                             "Off shows the game's own lighting for comparison; F1 toggles it at any time. Not saved: each start begins with them on.",
                .masterOff = "Off: the game's own lighting.",
                .keys = keys});
        performance::drawBudget(wanted);
        drawUpscaling(state, wanted);
        drawShadows(wanted);
        drawLighting(wanted);
        drawAtmosphere(wanted);
        drawCamera(wanted);
        drawDisplay(wanted);
        drawWorld(wanted);
        drawDiagnostics(state);
        if constexpr (DevTools) drawDeveloper();
    }

    if (enhancements != enhancementsOn()) setEnhancements(enhancements);
    if (wanted != before) applySettings(wanted);
}
}
