// The Phantom FX settings window in ReShade's overlay: fxui's frame budget window with Phantom FX's
// sections and settings. The costs and states come from menu_budget.cpp, the budget page from
// performance_menu.cpp.
#include "character.hpp"
#include "core.hpp"
#include "cutscenes.hpp"
#include "dev_tools.hpp"
#include "features/frame_generation/frame_generation.hpp"
#include "features/frame_generation/fsr_frame_generation.hpp"
#include "features/frame_generation/presenter.hpp"
#include "features/hdr/hdr.hpp"
#include "features/upscaling/upscaler.hpp"
#include "frame_log.hpp"
#include "fxui/fxui.hpp"
#include "game_lua.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "menu_budget.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "performance.hpp"
#include "travel.hpp"
#include "weather.hpp"

#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace phantom_fx::menu
{
namespace
{
using fxui::Choice;
using fxui::State;
using fxui::Tone;
using upscaler::FrameGenerationMode;
using upscaler::Mode;
using upscaler::Preset;
using upscaler::Upscaler;
using upscaler::label;
using upscaler::multiplier;
using upscaler::upscales;

// The upscaler's one line on what it is doing right now.
void drawStatus(const upscaler::Status& state, const Upscaling& wanted)
{
    const auto& name = state.upscalerName;
    const auto nativeMode = label(Mode::Dlaa, wanted.upscaler);
    // FSR has no presets.
    const auto preset = wanted.upscaler == Upscaler::Fsr ? std::string()
                        : state.rayReconstruction        ? std::format(", preset {}", label(wanted.reconstructionPreset))
                                                         : std::format(", preset {}", label(wanted.dlssPreset));
    if (!state.supportedBuild) fxui::statusLine(Tone::Bad, "This game version is not supported, so upscaling stays off.");
    else if (wanted.mode == Mode::Off) fxui::statusLine(Tone::Dim, "Upscaling is off. The game's FXAA is in use.");
    else if (state.failed) fxui::statusLine(Tone::Bad, std::format("{} stopped: {} Choose a mode or upscaler again to retry.", name, state.failure));
    else if (upscales(wanted.mode) && !state.upscalingAvailable)
        fxui::statusLine(Tone::Bad, std::format("Upscaling is unavailable (see PhantomFX.log); {} still works.", nativeMode));
    else if (core::currentDiagnostics().gameFxaa) fxui::statusLine(Tone::Warn, "Showing the game's FXAA for comparison (Diagnostics).");
    else if (!state.fxaaRecognised) fxui::statusLine(Tone::Warn, "Waiting for the game to draw its anti-aliasing.");
    else if (!state.running && state.lastFallback.empty()) fxui::statusLine(Tone::Warn, std::format("{} is waiting for the game's 3D view.", name));
    else if (!state.running) fxui::statusLine(Tone::Warn, std::format("{} is waiting: {}.", name, state.lastFallback));
    else if (state.renderWidth == state.outputWidth && state.renderHeight == state.outputHeight)
        fxui::statusLine(Tone::Ok, std::format("{} {} active at {}\xC3\x97{}{}.", name, nativeMode, state.outputWidth, state.outputHeight, preset));
    else
        fxui::statusLine(Tone::Ok, std::format("{} {} active: renders {}\xC3\x97{}, outputs {}\xC3\x97{}{}.", name, label(wanted.mode, wanted.upscaler),
            state.renderWidth, state.renderHeight, state.outputWidth, state.outputHeight, preset));

    if (wanted.upscaler != Upscaler::Dlss || !wanted.rayReconstruction || !state.running || state.rayReconstruction) return;

    fxui::statusLine(Tone::Warn, state.reconstructionAvailable ? "Ray Reconstruction waits for a ray-traced effect; DLSS runs meanwhile."
                                                               : "Ray Reconstruction could not run here (see PhantomFX.log); DLSS runs instead.");
}

// What the running game adds to a feature's item, beside its description.
struct ItemStatus
{
    std::optional<State> state;
    std::string message;
    std::vector<int> unsupported; // Choices this GPU cannot run
    float automatic{};            // What 0 stands for in an automatic slider (SliderInfo::automaticHint)
};

ItemStatus statusOf(const auto&)
{
    return {};
}

// The route the game started with runs until a restart. Until Streamline reports what the GPU
// supports, every DLSS mode is offered.
ItemStatus statusOf(const FrameGeneration& generation)
{
    const bool fsrRoute = presenter::route() == presenter::Route::Fsr;
    const bool sameRoute = (generation.mode == FrameGenerationMode::Fsr) == fsrRoute;
    const bool on = generation.mode != FrameGenerationMode::Off;
    ItemStatus status{.state = State::Off};
    if (presenter::active() && on && !sameRoute)
    {
        status.state = State::Restart;
        status.message = "Restart the game to switch frame generation.";
    }
    else if (presenter::active())
    {
        status.state = on ? State::Working : State::Off;
        status.message = fsrRoute ? fsr_frame_generation::status() : frame_generation::status();
    }
    else if (on)
    {
        status.state = State::Restart;
        const auto reason = presenter::status();
        status.message = presenter::installed() && sameRoute && !reason.empty() ? reason : "Restart the game to start frame generation.";
    }

    const auto supported = frame_generation::capabilities();
    for (int i = 0; i < static_cast<int>(FrameGenerationMode::Count); ++i)
    {
        const auto mode = static_cast<FrameGenerationMode>(i);
        bool unsupported{};
        if (mode == FrameGenerationMode::Dynamic) unsupported = supported.maxMultiplier && !supported.dynamic;
        else if (multiplier(mode)) unsupported = supported.maxMultiplier && multiplier(mode) > supported.maxMultiplier;
        if (unsupported) status.unsupported.push_back(i);
    }

    return status;
}

ItemStatus statusOf(const Hdr&)
{
    return {.automatic = hdr::peakNitsFor(0)};
}

// The settings that show in other features' items (SliderInfo::shownIn).
class SharedControls final : public SettingVisitor
{
public:
    void slider(float& value, const SliderInfo& info) override
    {
        if (!info.shownIn.empty()) sliders.push_back({&value, info});
    }

    std::vector<std::pair<float*, SliderInfo>> sliders;
};

// One section's features, each an item with its settings below it while it is on.
class MenuPage final : public SettingVisitor
{
public:
    MenuPage(Section section, const SharedControls& shared, bool isRayTracing) : section(section), shared(shared), isRayTracing(isRayTracing) {}

    ~MenuPage() { endItem(); }

    // Asked only when the next feature's item shows.
    void nextStatus(std::function<ItemStatus()> statusOfNext) { nextFeatureStatus = std::move(statusOfNext); }

    void feature(bool& on, const FeatureInfo& info) override { startItem(info, &on); }

    void feature(const FeatureInfo& info) override { startItem(info, nullptr); }

    void toggle(bool& value, const ToggleInfo& info) override
    {
        if (!open() || !info.visible || (info.needsRayTracing && !isRayTracing)) return;

        ImGui::BeginDisabled(!info.enabled);
        fxui::checkbox(info.label, value, info.tip);
        ImGui::EndDisabled();
    }

    void slider(float& value, const SliderInfo& info) override
    {
        if (open() && info.visible && info.shownIn.empty()) drawSlider(value, info);
    }

    void choice(int& value, const ChoiceInfo& info) override
    {
        if (!open() || !info.visible || (info.needsRayTracing && !isRayTracing)) return;

        std::vector<Choice<int>> choices;
        for (const auto& choice : info.choices)
        {
            const bool unsupported = std::ranges::find(status.unsupported, choice.value) != status.unsupported.end();
            choices.push_back({.value = choice.value, .label = choice.label, .enabled = !unsupported, .tip = unsupported ? "This GPU does not support it." : nullptr});
        }

        ImGui::BeginDisabled(!info.enabled);
        if (info.control == ChoiceControl::Dropdown) fxui::combo(info.label, value, std::span<const Choice<int>>(choices), info.tip);
        else fxui::segmented(info.label, value, std::span<const Choice<int>>(choices), info.tip);
        ImGui::EndDisabled();
    }

private:
    bool open() const { return item && *item; }

    // A switched feature without a budget line shows working while on; a budget line's state, switch
    // and message take the place of these (fxui.hpp).
    void startItem(const FeatureInfo& info, bool* on)
    {
        endItem();
        if (info.section != section) return;

        if (info.heading) fxui::subheading(info.heading);

        status = nextFeatureStatus ? nextFeatureStatus() : ItemStatus{};
        const auto state = status.state ? status.state : on ? std::optional(*on ? State::Working : State::Off) : std::nullopt;
        item.emplace(fxui::ItemSpec{.id = info.menuId, .name = info.name, .tech = info.tech, .tag = info.tag, .tip = info.tip, .summary = info.summary,
            .keywords = info.keywords, .toggle = on, .state = state, .message = status.message});
        effect = info.effect;
    }

    // Other features' settings shown in this item come last, each once a page: in the first of its
    // items that is open.
    void endItem()
    {
        if (open() && effect)
            for (auto& [value, info] : shared.sliders)
            {
                const bool shownHere = info.visible && std::ranges::find(info.shownIn, *effect) != info.shownIn.end();
                if (!shownHere || std::ranges::find(sharedShown, value) != sharedShown.end()) continue;

                drawSlider(*value, info);
                sharedShown.push_back(value);
            }

        item.reset();
        effect.reset();
    }

    void drawSlider(float& value, const SliderInfo& info)
    {
        ImGui::BeginDisabled(!info.enabled);
        if (info.automaticHint) drawAutomaticSlider(value, info);
        else fxui::slider(info.label, value, specOf(info, info.standard));
        if (info.hint) fxui::hint(info.hint);
        ImGui::EndDisabled();
    }

    // At 0 the slider shows what 0 stands for, which a reset also gives back.
    void drawAutomaticSlider(float& value, const SliderInfo& info)
    {
        const float automatic = std::clamp(status.automatic, info.min, info.max);
        float shown = value == 0 ? automatic : value;
        if (fxui::slider(info.label, shown, specOf(info, automatic))) value = shown == automatic ? 0 : shown;
        if (value == 0) fxui::hint(info.automaticHint);
    }

    static fxui::SliderSpec specOf(const SliderInfo& info, float standard)
    {
        return {.min = info.min, .max = info.max, .standard = standard, .format = info.format, .shown = info.readoutScale, .logarithmic = info.logarithmic,
            .onRelease = info.onRelease, .tip = info.tip};
    }

    Section section;
    const SharedControls& shared;
    bool isRayTracing; // A ray-traced effect is on in the settings

    std::function<ItemStatus()> nextFeatureStatus;
    ItemStatus status;
    std::optional<fxui::Item> item;
    std::optional<Effect> effect;
    std::vector<const float*> sharedShown;
};

// The section's features, in the menu's order.
void drawFeatures(Section section, Settings& wanted)
{
    SharedControls shared;
    describe(wanted, shared);
    MenuPage page(section, shared, planEffects(wanted, {}).tracesScene());
    forEachFeature(wanted, [&](auto& feature) {
        page.nextStatus([&feature] { return statusOf(feature); });
        describe(feature, page);
    });
}

void drawUpscaling(const upscaler::Status& state, Settings& wanted)
{
    const fxui::Page page(Section::Upscaling, "Anti-aliasing and upscaling in place of the game's FXAA, and frames generated between the rendered ones.");
    if (!page) return;

    drawStatus(state, wanted.upscaling);
    drawFeatures(Section::Upscaling, wanted);
}

void drawShadows(Settings& wanted)
{
    const fxui::Page page(Section::Shadows, "Shadows of the sun and of lamps. The ray-traced ones replace the screen-space ones while on.");
    if (!page) return;

    drawFeatures(Section::Shadows, wanted);
}

void drawLighting(Settings& wanted)
{
    const fxui::Page page(Section::Lighting,
        "Light that bounces between surfaces, and the shade where they meet. Global illumination replaces both screen-space effects while on.");
    if (!page) return;

    drawFeatures(Section::Lighting, wanted);
}

void drawAtmosphere(Settings& wanted)
{
    const fxui::Page page(Section::Atmosphere, "Fog with the sun shadowed in it, and the sky.");
    if (!page) return;

    drawFeatures(Section::Atmosphere, wanted);
}

void drawCamera(Settings& wanted)
{
    const fxui::Page page(Section::Camera, "Depth of field, motion blur and the lens of photo mode.");
    if (!page) return;

    drawFeatures(Section::Camera, wanted);
}

void drawDisplay(Settings& wanted)
{
    const fxui::Page page(Section::Display, "HDR output, and the frame rate cap.");
    if (!page) return;

    drawFeatures(Section::Display, wanted);
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

void drawWorld()
{
    const fxui::Page page(Section::World, "The world's weather and time, and who you play.");
    if (!page) return;

    drawWeather();
    drawCharacter();
}

void drawDiagnostics(const upscaler::Status& state)
{
    const fxui::Page page(Section::Diagnostics, "For troubleshooting. Leave these at their defaults.");
    if (!page) return;

    auto options = core::currentDiagnostics();
    bool changed{};
    if (const fxui::Item item({.id = "debugView", .name = "Debug view", .keywords = "motion vectors object motion shadow rays probe"}); item)
    {
        using enum DebugView;
        constexpr Choice<DebugView> views[]{{FinalImage, "Final image"}, {MotionVectors, "Motion vectors"}, {ObjectMotion, "Object motion (red)"},
            {ShadowRays, "Ray-traced shadow rays"}, {LightHistory, "Global illumination history"},
            {LightValidation, "Global illumination denoiser (NRD)"}, {ShadowStarts, "Shadow ray starts"}};
        changed |= fxui::combo("Debug view", options.debugView, views,
            "Ray-traced shadow rays: with ray-traced shadows and an upscaler on, what blocks each pixel's ray to the sun's centre, over the image. The "
            "cross is the probe: its sun ray and, with ray-traced lights on, sun or none, the strongest lamps' rays and what blocks each. Its "
            "findings show below (and, in a dev build, go to PhantomFX.log whenever they change).");
        if (options.debugView == LightHistory)
            fxui::note(Tone::Dim, "With global illumination and an upscaler on: red where the denoiser started a pixel's light over this frame or "
                                  "the last two, yellow where it holds under ten frames. Clear: a full history.");
        if (options.debugView == LightValidation)
            fxui::note(Tone::Dim, "With global illumination and an upscaler on, NRD's own checks of the denoiser's inputs, in a 4 x 4 grid. Top "
                                  "row: world grid, roughness, view depth (red: past the denoising range), motion against a still scene (black "
                                  "where right, magenta where it holds the jitter). Second row: normals. Third row, left: frames of history "
                                  "(red: started over).");
        if (options.debugView == ShadowStarts)
            fxui::note(Tone::Dim, "With ray-traced shadows on, where each pixel's shadow rays leave, against what the game drew. Grey: from the depth, "
                                  "the scene having no surface there. Blue: the scene's surface faces another way. Red: it lies nearer than the "
                                  "game's, cyan: farther (clear under 5 mm, full from 3 cm). Clear: on the game's surface.");
        if (options.debugView == ShadowRays)
            fxui::note(Tone::Dim, "Lavender: the game's models and characters. Brown: the terrain. Yellow: cut-out geometry. Olive: cut-out geometry "
                                  "without its texture, left to the shadow map. Magenta: anything else only the whole ray finds. Clear: the sun reaches.");
        if (options.debugView == ShadowRays || options.debugView == ShadowStarts)
        {
            if (const auto probe = ray_tracing::shadowProbe(); !probe.empty()) fxui::note(Tone::Dim, probe);
            else fxui::note(Tone::Warn, "Waiting for ray-traced shadows or lights (on, with an upscaler). The sun's rays need a sun: indoors, only "
                                        "the lamps' probe shows.");
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
            "depth, magenta and black stripes where it lacks what the game drew, yellow where it has something in the sky.");
        if (const auto scene = ray_tracing::status(); options.showTracedScene && !scene.empty()) fxui::note(Tone::Dim, scene);
        constexpr Choice<int> volumes[]{{0, "The game's volumes"}, {1, "Shadow-mapped lights beyond"}, {2, "Every light beyond"}};
        changed |= fxui::combo("Light volumes", options.lightVolumes, volumes,
            "With ray-traced lights on: the game bounds each light by a box (a spot by its cone within one), and they stop at it as the "
            "game's do. Beyond, a light shines over all its reach, stopped by the rays alone: those the game gives a shadow map, or every one.");
        changed |= fxui::checkbox("Shadows from every surface (old)", options.twoSidedShadows,
            "Ray-traced shadows of the sun and lamps: every surface in the scene blocks light, from both sides, as before. By default they take "
            "what the game's shadow maps hold: an opaque surface casts only from its back as the light sees it, and meshes the game shows in the "
            "main view only cast nothing, so windows let light through as the game's do.");
    }
    if (changed) core::applyDiagnostics(options);

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

// A tool that runs Lua has nothing to run on a game version without it.
fxui::ItemSpec luaTool(fxui::ItemSpec spec)
{
    if (game_lua::isSupported()) return spec;

    spec.state = State::Unavailable;
    spec.message = "Unavailable for this game version.";

    return spec;
}

// Free roam in another of the game's maps.
void drawTravel()
{
    const fxui::Item item(luaTool({.id = "travel",
        .name = "Travel",
        .summary = "Free roam in another map, after one loading screen.",
        .keywords = "teleport map location Afghanistan Africa Mother Base free roam"}));
    if (!item || !game_lua::isSupported()) return;

    std::array<fxui::Option, static_cast<std::size_t>(travel::Map::Count)> maps{};
    for (std::size_t i = 0; i < maps.size(); ++i) maps[i].label = travel::name(static_cast<travel::Map>(i));
    const int pressed = fxui::segmented("Go to", -1, maps,
        "Leaves free roam as the helicopter would and arrives on foot: in Afghanistan and Africa where the game puts a player found off the map, "
        "at Mother Base on the Command platform. The game saves on the way, as it does when leaving free roam.");
    if (pressed >= 0) travel::go(static_cast<travel::Map>(pressed));
}

// Any of the game's cutscenes, in its own mission.
void drawCutscenes()
{
    const fxui::Item item(luaTool({.id = "cutscenes",
        .name = "Cutscenes",
        .summary = "Plays any cutscene the game's scripts name, after one loading screen.",
        .keywords = "demo movie story mission play watch scene"}));
    if (!item || !game_lua::isSupported()) return;

    static std::optional<std::size_t> selected;
    const auto labels = cutscenes::labels();
    const char* const preview = selected ? labels[*selected].data() : "Pick a cutscene";
    if (const auto picked = fxui::searchCombo("Cutscene", preview, labels, selected)) selected = picked;

    if (fxui::button("Play",
            "From free roam or the ACC. A story cutscene loads its mission and ends in the ACC, with no mission clear; a Mother Base one ends in free "
            "roam there. The game saves on the way, as it does when leaving free roam. The story stays where it is, unless the game plays another "
            "Mother Base cutscene in its place, which a toast says. Cutscenes start in the mission's own time and weather, not always the ones they "
            "have in the story.",
            selected.has_value()))
        cutscenes::play(*selected);
}

// Lua typed here runs in the game's own script state.
void drawLuaConsole()
{
    const fxui::Item item(luaTool({.id = "luaConsole",
        .name = "Lua console",
        .summary = "Runs Lua in the game's own scripts at the next frame: an expression shows its value, print() its lines.",
        .keywords = "script execute run code vars gvars mvars svars TppMission"}));
    if (!item || !game_lua::isSupported()) return;

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
    drawCutscenes();
    drawLuaConsole();
    if constexpr (DiagnosticLog)
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

void draw()
{
    auto wanted = core::currentSettings();
    const auto before = wanted;
    const auto state = upscaler::status();
    bool enhancements = core::enhancementsOn();
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
        drawWorld();
        drawDiagnostics(state);
        if constexpr (DevTools) drawDeveloper();
    }

    if (enhancements != core::enhancementsOn()) core::setEnhancements(enhancements);
    if (wanted != before) core::applySettings(wanted);
}
}
