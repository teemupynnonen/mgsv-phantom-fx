// The Phantom FX settings window in ReShade's overlay: fxui's frame budget window with Phantom FX's
// sections and settings. The costs and states come from menu_budget.cpp, the Budget tab from
// performance_menu.cpp.
#include "character.hpp"
#include "core.hpp"
#include "dev/dev.hpp"
#include "dev_tools.hpp"
#include "features/frame_generation/frame_generation.hpp"
#include "features/frame_generation/fsr_frame_generation.hpp"
#include "features/frame_generation/presenter.hpp"
#include "features/hdr/hdr.hpp"
#include "features/upscaling/upscaler.hpp"
#include "frame_log.hpp"
#include "fxui/fxui.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "menu_budget.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "performance.hpp"
#include "weather.hpp"

#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <algorithm>
#include <array>
#include <format>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
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
using upscaler::Upscaler;
using upscaler::multiplier;

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

// What each toggle and choice is by default, with a lighting step applied: what its reset gives back.
// Sliders carry theirs (SliderInfo::standard).
class Standards final : public SettingVisitor
{
public:
    void toggle(bool& value, const ToggleInfo& info) override { values.emplace(info.iniKey, value ? 1 : 0); }

    void choice(int& value, const ChoiceInfo& info) override { values.emplace(info.iniKey, value); }

    std::optional<int> of(std::string_view iniKey) const
    {
        const auto found = values.find(iniKey);
        return found == values.end() ? std::nullopt : std::optional(found->second);
    }

    std::optional<bool> of(const ToggleInfo& info) const
    {
        const auto standard = of(info.iniKey);
        return standard ? std::optional(*standard != 0) : std::nullopt;
    }

private:
    // The keys are the features' string literals, so they outlive the settings described.
    std::map<std::string_view, int> values;
};

// Settings{} with `step` applied, so the Ray Reconstruction Ultra asks for is no change.
const Standards& standardsOf(LightingStep step)
{
    static const auto described = [] {
        std::array<Standards, static_cast<std::size_t>(LightingStep::Count)> byStep;
        for (std::size_t i = 0; i < byStep.size(); ++i)
        {
            Settings settings;
            applyLightingStep(settings, static_cast<LightingStep>(i));
            describe(settings, byStep[i]);
        }
        return byStep;
    }();

    return described[static_cast<std::size_t>(step)];
}

// What the items are drawn against this frame.
struct MenuFrame
{
    const Standards& standards; // With the lighting step the menu shows
    std::string_view resetting; // The item whose "Reset all" was pressed the frame before
};

// "Reset all" pressed in an item puts its settings back on the next frame (MenuFrame::resetting).
std::string resetNext;

constexpr std::string_view RaysId = "rays", ModeId = "lighting";

// As its readout shows it, so a value the slider cannot tell from its default counts as the default.
bool isAtStandard(float value, const SliderInfo& info)
{
    const auto readout = [&](float v) {
        char text[64];
        std::snprintf(text, sizeof(text), info.format, v * info.readoutScale);
        return std::string(text);
    };

    return readout(value) == readout(info.standard);
}

// The items with a setting off its default, for the dot on their lines. A setting shown in other
// features' items counts in each of them (SliderInfo::shownIn).
class ChangedItems final : public SettingVisitor
{
public:
    explicit ChangedItems(const Standards& standards) : standards(standards) {}

    void feature(bool&, const FeatureInfo& info) override { current = info.menuId; }

    void feature(const FeatureInfo& info) override { current = info.menuId; }

    void toggle(bool& value, const ToggleInfo& info) override
    {
        if (info.visible && value != standards.of(info).value_or(value)) mark(itemOf(info.isRaySetting));
    }

    void choice(int& value, const ChoiceInfo& info) override
    {
        if (info.visible && !info.isItemValue && value != standards.of(info.iniKey).value_or(value)) mark(itemOf(info.isRaySetting));
    }

    void slider(float& value, const SliderInfo& info) override
    {
        if (!info.visible || isAtStandard(value, info)) return;

        if (info.shownIn.empty()) mark(current);
        else std::ranges::copy(info.shownIn, std::back_inserter(effects));
    }

    bool has(std::string_view id, std::optional<Effect> effect = {}) const
    {
        return std::ranges::find(ids, id) != ids.end() || (effect && std::ranges::find(effects, *effect) != effects.end());
    }

private:
    std::string_view itemOf(bool isRaySetting) const { return isRaySetting ? RaysId : current; }

    void mark(std::string_view id) { ids.push_back(id); }

    const Standards& standards;

    std::string_view current;
    std::vector<std::string_view> ids;
    std::vector<Effect> effects;
};

const char* const ResetAllTip = "Every setting here back to its default.";

// A toggle in an item: the item's "Reset all" puts it back, and it shows while the item is open.
void toggleInItem(bool& value, const ToggleInfo& info, const Standards& standards, bool isResetting, bool isShown)
{
    const auto standard = standards.of(info);
    if (isResetting && standard) value = *standard;
    if (!isShown || !info.visible) return;

    ImGui::BeginDisabled(!info.enabled);
    fxui::checkbox(info.label, value, info.tip, standard);
    ImGui::EndDisabled();
}

// The choices as the menu offers them: those in `unsupported`, which this GPU cannot run, greyed out.
std::vector<Choice<int>> choicesOf(const ChoiceInfo& info, std::span<const int> unsupported)
{
    std::vector<Choice<int>> choices;
    for (const auto& choice : info.choices)
    {
        const bool isUnsupported = std::ranges::find(unsupported, choice.value) != unsupported.end();
        choices.push_back({.value = choice.value, .label = choice.label, .enabled = !isUnsupported, .tip = isUnsupported ? "This GPU does not support it." : nullptr});
    }

    return choices;
}

// A choice in an item, as toggleInItem() takes a toggle.
void choiceInItem(int& value, const ChoiceInfo& info, std::span<const int> unsupported, const Standards& standards, bool isResetting, bool isShown)
{
    const auto standard = standards.of(info.iniKey);
    if (isResetting && standard) value = *standard;
    if (!isShown || !info.visible) return;

    const auto choices = choicesOf(info, unsupported);
    ImGui::BeginDisabled(!info.enabled);
    if (info.control == ChoiceControl::Dropdown) fxui::combo(info.label, value, std::span<const Choice<int>>(choices), info.tip, standard);
    else fxui::segmented(info.label, value, std::span<const Choice<int>>(choices), info.tip, standard);
    ImGui::EndDisabled();
}

// A choice as its item's value, on the item's line (ChoiceInfo::isItemValue).
void drawItemValue(int& value, const ChoiceInfo& info, std::span<const int> unsupported)
{
    const auto choices = choicesOf(info, unsupported);
    std::vector<fxui::Option> options;
    for (const auto& choice : choices) options.push_back({.label = choice.label, .enabled = choice.enabled, .tip = choice.tip});
    const auto selected = std::ranges::find(choices, value, &Choice<int>::value);

    ImGui::BeginDisabled(!info.enabled);
    if (const int picked = fxui::itemChoice(static_cast<int>(selected - choices.begin()), options); picked >= 0) value = choices[static_cast<std::size_t>(picked)].value;
    ImGui::EndDisabled();
}

// The button under an item's settings while one of them is off its default.
void drawResetAll(std::string_view id, const char* tip = ResetAllTip)
{
    fxui::gap(4);
    if (fxui::button("Reset all", tip)) resetNext = id;
}

// Which features of a page get an item.
using FeatureFilter = std::function<bool(const FeatureInfo&)>;

// One section's features, each an item that folds out to its settings. The rays' settings are left
// to the Rays item (drawRays).
class MenuPage final : public SettingVisitor
{
public:
    MenuPage(Section section, const SharedControls& shared, const ChangedItems& changed, const MenuFrame& frame, FeatureFilter shows)
        : section(section), shared(shared), changed(changed), frame(frame), shows(std::move(shows))
    {
    }

    ~MenuPage() { endItem(); }

    // Asked only when the next feature's item shows.
    void nextStatus(std::function<ItemStatus()> statusOfNext) { nextFeatureStatus = std::move(statusOfNext); }

    void feature(bool& on, const FeatureInfo& info) override { startItem(info, &on); }

    void feature(const FeatureInfo& info) override { startItem(info, nullptr); }

    void toggle(bool& value, const ToggleInfo& info) override
    {
        if (!info.isRaySetting) toggleInItem(value, info, frame.standards, isResetting(), isOpen());
    }

    void slider(float& value, const SliderInfo& info) override
    {
        if (!info.shownIn.empty()) return;

        if (isResetting()) value = info.standard;
        if (isOpen() && info.visible) drawSlider(value, info);
    }

    void choice(int& value, const ChoiceInfo& info) override
    {
        if (info.isRaySetting) return;

        if (info.isItemValue)
        {
            if (item) drawItemValue(value, info, status.unsupported);
            return;
        }

        choiceInItem(value, info, status.unsupported, frame.standards, isResetting(), isOpen());
    }

private:
    bool isOpen() const { return item && *item; }

    bool isResetting() const { return item && frame.resetting == itemId; }

    // A switched feature without a budget line shows working while on; a budget line's state, switch
    // and message take the place of these (fxui.hpp). A feature that shares the item's switch goes on
    // in that item (FeatureInfo::menuId).
    void startItem(const FeatureInfo& info, bool* on)
    {
        if (item && itemId == info.menuId) return;

        endItem();
        if (info.section != section || !shows(info)) return;

        status = nextFeatureStatus ? nextFeatureStatus() : ItemStatus{};
        const auto state = status.state ? status.state : on ? std::optional(*on ? State::Working : State::Off) : std::nullopt;
        item.emplace(fxui::ItemSpec{.id = info.menuId, .name = info.name, .tech = info.tech, .tag = info.tag, .tip = info.tip, .summary = info.summary,
            .keywords = info.keywords, .isChanged = changed.has(info.menuId, info.effect), .toggle = on, .state = state, .message = status.message});
        itemId = info.menuId;
        effect = info.effect;
    }

    // Other features' settings shown in this item come last, each once a page: in the first of its
    // items that is open. Then the reset of them all.
    void endItem()
    {
        if (item && effect)
            for (auto& [value, info] : shared.sliders)
            {
                const bool isShownHere = info.visible && std::ranges::find(info.shownIn, *effect) != info.shownIn.end();
                if (!isShownHere) continue;

                if (isResetting()) *value = info.standard;
                if (!isOpen() || std::ranges::find(sharedShown, value) != sharedShown.end()) continue;

                drawSlider(*value, info);
                sharedShown.push_back(value);
            }
        if (isOpen() && changed.has(itemId, effect)) drawResetAll(itemId);

        item.reset();
        itemId = {};
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
    const ChangedItems& changed;
    const MenuFrame& frame;
    FeatureFilter shows;

    std::function<ItemStatus()> nextFeatureStatus;
    ItemStatus status;
    std::optional<fxui::Item> item;
    std::string_view itemId;
    std::optional<Effect> effect;
    std::vector<const float*> sharedShown;
};

// The section's features, in the menu's order: those `shows` takes, by default all.
void drawFeatures(Section section, Settings& wanted, const MenuFrame& frame, FeatureFilter shows = [](const FeatureInfo&) { return true; })
{
    SharedControls shared;
    describe(wanted, shared);
    ChangedItems changed(frame.standards);
    describe(wanted, changed);
    MenuPage page(section, shared, changed, frame, std::move(shows));
    forEachFeature(wanted, [&](auto& feature) {
        page.nextStatus([&feature] { return statusOf(feature); });
        describe(feature, page);
    });
}

void drawSection(Section section, Settings& wanted, const MenuFrame& frame)
{
    const fxui::Page page(section);
    if (!page) return;

    drawFeatures(section, wanted, frame);
}

// The settings the ray-traced effects share, from the features that hold them (ToggleInfo::isRaySetting).
class RaySettings final : public SettingVisitor
{
public:
    RaySettings(bool isShown, const MenuFrame& frame) : isShown(isShown), isResetting(frame.resetting == RaysId), standards(frame.standards) {}

    void toggle(bool& value, const ToggleInfo& info) override
    {
        if (info.isRaySetting) toggleInItem(value, info, standards, isResetting, isShown);
    }

    void choice(int& value, const ChoiceInfo& info) override
    {
        if (info.isRaySetting) choiceInItem(value, info, {}, standards, isResetting, isShown);
    }

private:
    bool isShown{}, isResetting{};
    const Standards& standards;
};

void drawRays(Settings& wanted, const MenuFrame& frame)
{
    ChangedItems changed(frame.standards);
    describe(wanted, changed);
    const bool isChanged = changed.has(RaysId);
    const fxui::Item item({.id = RaysId.data(),
        .name = "Rays",
        .summary = "Shared by every ray-traced effect.",
        .keywords = "ray tracing DLSS Ray Reconstruction preset denoiser opacity micromaps short rays",
        .isChanged = isChanged});

    RaySettings settings(static_cast<bool>(item), frame);
    describe(wanted, settings);
    if (item && isChanged) drawResetAll(RaysId);
}

struct StepText
{
    const char* label;
    const char* detail;
    const char* description;
};

constexpr std::array<StepText, static_cast<std::size_t>(LightingStep::Count)> StepTexts{{
    {"Off", "Any GPU", "The game's own shadows and light."},
    {"Screen-space", "Any GPU", "Softer shadows and bounce light, from what is on screen."},
    {"Ray traced", "A GPU that ray traces: NVIDIA RTX, AMD Radeon RX 6000 and newer, Intel Arc", "Shadows and bounce light traced through the whole scene."},
    {"Ray traced: Ultra", "NVIDIA RTX, with DLSS", "Ray traced, plus traced reflections, sunlight and lamplight."},
}};

constexpr const char* UltraNeedsDlss = "Needs DLSS Ray Reconstruction. Under Image > Upscaler, pick DLSS and a mode other than Off.";

// A step shows the items of the effects it adds. Ultra also shows bounce light's: path tracing needs
// it, and replaces the other ray-traced effects.
bool isShownInStep(Effect effect, LightingStep step)
{
    const auto added = effectsAddedBy(step);
    return std::ranges::find(added, effect) != added.end() || (step == LightingStep::Ultra && effect == Effect::GlobalIllumination);
}

// What is switched off that `step` switches on, by the items' names: "Contact shadows and Bounce light (SSILVB)".
std::string offInStep(const Settings& wanted, LightingStep step)
{
    auto asStep = wanted;
    applyLightingStep(asStep, step);
    auto asWanted = wanted; // switchOf() takes settings it may change
    std::vector<std::string> names;
    for (const auto effect : AllEffects)
    {
        // A feature without a key of its own goes with the item before it.
        const auto& feature = featureOf(effect);
        if (feature.iniKey.empty() || switchOf(asStep, effect) == switchOf(asWanted, effect)) continue;

        names.push_back(feature.tech ? std::format("{} ({})", feature.name, feature.tech) : std::string(feature.name));
    }

    if (asStep.upscaling.rayReconstruction != wanted.upscaling.rayReconstruction) names.emplace_back("Ray Reconstruction");

    std::string off;
    for (std::size_t i = 0; i < names.size(); ++i) off += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];

    return off;
}

// The lighting step, as the first line's value, then the items of the effects it runs. During a
// search, every lighting item.
void drawLighting(Settings& wanted, const fxui::Budget& budget, const MenuFrame& frame)
{
    const fxui::Page page(Section::Lighting);
    if (!page) return;

    if (frame.resetting == ModeId) applyLightingStep(wanted, lightingStepOf(wanted));

    const auto step = lightingStepOf(wanted);
    const auto& text = StepTexts[static_cast<std::size_t>(step)];
    const bool isMixed = !isLightingAsStep(wanted);
    const bool canRunUltra = wanted.upscaling.upscaler == Upscaler::Dlss && wanted.upscaling.mode != Mode::Off;
    const bool isUltraStopped = step == LightingStep::Ultra && !canRunUltra;
    {
        const fxui::Item item({.id = ModeId.data(),
            .name = "Mode",
            .summary = text.description,
            .keywords = "lighting step off screen-space ray traced ultra path tracing",
            .isChanged = isMixed,
            .state = isUltraStopped ? std::optional(State::NotRunning) : step == LightingStep::Game ? std::optional(State::Off) : std::nullopt,
            .message = isUltraStopped ? UltraNeedsDlss : "",
            .ms = budget.total(static_cast<int>(Section::Lighting))});

        std::array<fxui::Option, static_cast<std::size_t>(LightingStep::Count)> options;
        for (std::size_t i = 0; i < options.size(); ++i) options[i] = {.label = StepTexts[i].label, .tip = StepTexts[i].description};
        if (!canRunUltra)
        {
            auto& ultra = options[static_cast<std::size_t>(LightingStep::Ultra)];
            ultra.enabled = false;
            ultra.tip = UltraNeedsDlss;
        }
        if (const int picked = fxui::itemChoice(static_cast<int>(step), options); picked >= 0) applyLightingStep(wanted, static_cast<LightingStep>(picked));

        if (item)
        {
            fxui::fact("Needs", text.detail);
            if (isMixed)
            {
                fxui::note(fxui::Tone::Warn, std::format("Your own mix: {} off.", offInStep(wanted, step)));
                drawResetAll(ModeId, "Every lighting effect back as this mode sets it.");
            }
        }
    }

    // Also an effect a step beneath adds that nothing replaces, on or off: such as SSILVB with bounce
    // light off, which runs in its place.
    const auto plan = planEffects(wanted, {});
    const auto isShown = [&](Effect effect) {
        if (isShownInStep(effect, step)) return true;

        for (int below = 1; below < static_cast<int>(step); ++below)
        {
            const auto added = effectsAddedBy(static_cast<LightingStep>(below));
            if (std::ranges::find(added, effect) != added.end()) return !plan.replacers[effect];
        }

        return false;
    };
    // Features that share a switch show with either effect, under the first one's name (FeatureInfo::menuId).
    drawFeatures(Section::Lighting, wanted, frame, [&](const FeatureInfo& feature) {
        if (fxui::isSearching()) return true;

        return std::ranges::any_of(AllEffects, [&](Effect effect) { return std::string_view(featureOf(effect).menuId) == feature.menuId && isShown(effect); });
    });
    if (fxui::isSearching() || step == LightingStep::RayTraced || step == LightingStep::Ultra) drawRays(wanted, frame);
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
        .summary = "Set the clock and the weather.",
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
        .summary = "Play as anyone, in any outfit.",
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

void drawSandbox()
{
    const fxui::Page page(Section::Sandbox);
    if (!page) return;

    drawWeather();
    drawCharacter();
}

void drawDiagnostics(const upscaler::Status& state)
{
    const fxui::Page page(Section::Diagnostics);
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
            .keywords = "jitter FXAA contact shadows GTVBAO SSILVB global illumination scene BVH only"});
        item)
    {
        changed |= fxui::checkbox("Jitter off", options.noJitter, "The upscaler gets the same pixel positions every frame. Edges stay aliased.");
        changed |= fxui::checkbox("Game FXAA (comparison)", options.gameFxaa);
        changed |= fxui::checkbox("Sun contact shadows only", options.onlyScreenSpaceShadows,
            "With sun contact shadows on, the sun is shadowed by them alone, without the shadow map.");
        changed |= fxui::checkbox("GTVBAO only", options.showAmbientOcclusion, "With GTVBAO on, the scene shows its occlusion alone: white where nothing occludes.");
        changed |= fxui::checkbox("SSILVB only", options.showIndirectLight, "With SSILVB on, the scene shows the light it adds alone: black where nothing bounces.");
        changed |= fxui::checkbox("Global illumination only", options.showGlobalIllumination, "With global illumination on, the scene shows its indirect light alone.");
        changed |= fxui::checkbox("GI BVH scene", options.showTracedScene,
            "With global illumination on, the scene shows the BVH's geometry, coloured by its normals and drawn alone: not through the game's "
            "resolve, so no albedo or specular of the game's is on it. Red where it disagrees with the game's depth, magenta and black stripes where "
            "it lacks what the game drew, yellow where it has something in the sky.");
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

void draw()
{
    auto wanted = core::currentSettings();
    const auto before = wanted;
    const auto state = upscaler::status();
    bool enhancements = core::enhancementsOn();
    const auto resetting = std::exchange(resetNext, {});
    const MenuFrame frame{.standards = standardsOf(lightingStepOf(wanted)), .resetting = resetting};
    // The costs show in every part of the window, so its passes are timed while it is open.
    performance::watch();
    const auto budget = menu::budget(wanted, performance::status().costs);

    {
        const fxui::Window window(menu::sections(), budget,
            {.name = "Phantom FX",
                .master = &enhancements,
                .masterLabel = "Visual enhancements",
                .masterKey = "F1",
                .masterTip = "Soft shadows, contact shadows, GTVBAO, SSILVB, the volumetric fog and the ray tracing together, as set in their sections. "
                             "Off shows the game's own lighting for comparison; F1 toggles it at any time. Not saved: each start begins with them on.",
                .masterOff = "Off: the game's own lighting."});
        drawLighting(wanted, budget, frame);
        drawSection(Section::Image, wanted, frame);
        drawSection(Section::World, wanted, frame);
        drawSection(Section::Camera, wanted, frame);
        drawSandbox();
        performance::drawBudget();
        drawDiagnostics(state);
        if constexpr (DevTools) dev::drawPage();
    }

    if (enhancements != core::enhancementsOn()) core::setEnhancements(enhancements);
    if (wanted != before) core::applySettings(wanted);
}
}
