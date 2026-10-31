#include "phantom_fx_settings.hpp"

#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <locale>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace phantom_fx
{
namespace
{
std::string_view trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t\r");
    return begin == text.npos ? std::string_view{} : text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

template<class T> bool number(std::string_view text, T& result)
{
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;

    result = value;

    return true;
}

// Each leaves the setting as it is when the text is not valid.
void readFlag(std::string_view value, bool& result)
{
    if (value == "0" || value == "1") result = value == "1";
}

void readFloat(std::string_view value, float& result, float low, float high)
{
    float parsed{};
    if (number(value, parsed) && std::isfinite(parsed)) result = std::clamp(parsed, low, high);
}

// The choice PhantomFX.ini names: by its key, or by its value when it has none.
const SettingChoice* choiceNamed(const ChoiceInfo& info, std::string_view text)
{
    for (const auto& choice : info.choices)
    {
        int value{};
        const bool named = choice.iniKey.empty() ? number(text, value) && value == choice.value : text == choice.iniKey;
        if (named) return &choice;
    }

    return nullptr;
}

using IniValues = std::map<std::string, std::string, std::less<>>;

class IniReader final : public SettingVisitor
{
public:
    explicit IniReader(const IniValues& values) : values(values) {}

    void feature(bool& on, const FeatureInfo& info) override { readFlag(textOf(info.iniKey), on); }

    void toggle(bool& value, const ToggleInfo& info) override { readFlag(textOf(info.iniKey), value); }

    void slider(float& value, const SliderInfo& info) override
    {
        const auto text = textOf(info.iniKey);
        float parsed{};
        if (info.automaticHint && number(text, parsed) && parsed == 0) value = 0;
        else readFloat(text, value, info.min, info.max);
    }

    void choice(int& value, const ChoiceInfo& info) override
    {
        if (const auto choice = choiceNamed(info, textOf(info.iniKey))) value = choice->value;
    }

private:
    // Empty for a key the file leaves out, which leaves its setting as it is.
    std::string_view textOf(std::string_view key) const
    {
        const auto found = values.find(key);
        return found == values.end() ? std::string_view{} : std::string_view(found->second);
    }

    const IniValues& values;
};

class IniWriter final : public SettingVisitor
{
public:
    explicit IniWriter(std::ostream& output) : output(output) {}

    void feature(bool& on, const FeatureInfo& info) override { write(info.iniKey, on); }

    void toggle(bool& value, const ToggleInfo& info) override { write(info.iniKey, value); }

    void slider(float& value, const SliderInfo& info) override { write(info.iniKey, value); }

    void choice(int& value, const ChoiceInfo& info) override
    {
        const auto choice = std::ranges::find(info.choices, value, &SettingChoice::value);
        if (choice->iniKey.empty()) write(info.iniKey, value);
        else write(info.iniKey, choice->iniKey);
    }

private:
    template<class T> void write(std::string_view key, const T& value) { output << key << '=' << value << '\n'; }

    std::ostream& output;
};

class RangeCheck final : public SettingVisitor
{
public:
    void slider(float& value, const SliderInfo& info) override
    {
        const bool automatic = info.automaticHint && value == 0;
        valid = valid && (automatic || (std::isfinite(value) && value >= info.min && value <= info.max));
    }

    void choice(int& value, const ChoiceInfo& info) override
    {
        valid = valid && std::ranges::find(info.choices, value, &SettingChoice::value) != info.choices.end();
    }

    bool valid = true;
};

class EffectSwitches final : public SettingVisitor
{
public:
    void feature(bool& on, const FeatureInfo& info) override
    {
        if (info.effect) switches[*info.effect] = &on;
    }

    ByEffect<bool*> switches;
};

class EffectFeatures final : public SettingVisitor
{
public:
    void feature(bool&, const FeatureInfo& info) override
    {
        if (info.effect) features[*info.effect] = info;
    }

    ByEffect<FeatureInfo> features;
};

// These take the settings by value: describe() takes settings it may change.
bool inRange(Settings settings)
{
    RangeCheck check;
    describe(settings, check);

    return check.valid;
}

void writeDescribed(std::ostream& output, Settings settings)
{
    IniWriter writer(output);
    describe(settings, writer);
}
}

void describe(Settings& settings, SettingVisitor& visit)
{
    forEachFeature(settings, [&](auto& feature) { describe(feature, visit); });
}

const FeatureInfo& featureOf(Effect effect)
{
    static const auto features = [] {
        Settings settings;
        EffectFeatures described;
        describe(settings, described);
        return described.features;
    }();

    return features[effect];
}

bool& switchOf(Settings& settings, Effect effect)
{
    EffectSwitches described;
    describe(settings, described);

    return *described.switches[effect];
}

void applyPreset(Settings& settings, EffectPreset preset)
{
    const bool screenSpace = preset == EffectPreset::ScreenSpace;
    settings.softShadows.on = screenSpace;
    settings.sunContactShadows.on = screenSpace;
    settings.lampContactShadows.on = screenSpace;
    settings.ambientOcclusion.on = screenSpace;
    settings.indirectLight.on = screenSpace;

    settings.rayTracedShadows.on = !screenSpace;
    settings.rayTracedLights.on = !screenSpace;
    settings.globalIllumination.on = !screenSpace;

    settings.frameRate.unlocked = true;
    settings.volumetricFog.on = true;
    settings.volumetricFog.look = AtmosphereLook::Physical;
}

std::optional<EffectPreset> presetOf(const Settings& settings)
{
    for (const auto preset : {EffectPreset::ScreenSpace, EffectPreset::RayTraced})
    {
        auto switched = settings;
        applyPreset(switched, preset);
        if (switched == settings) return preset;
    }

    return std::nullopt;
}

Settings firstLaunchSettings()
{
    Settings settings{.upscaling = {.mode = upscaler::Mode::Quality}};
    applyPreset(settings, EffectPreset::ScreenSpace);

    return settings;
}

EffectState EffectPlan::operator[](Effect effect) const
{
    return states[effect];
}

bool EffectPlan::runs(Effect effect) const
{
    return states[effect] == EffectState::Running;
}

bool EffectPlan::tracesScene() const
{
    return std::ranges::any_of(RayTracedEffects, [this](Effect effect) { return runs(effect); });
}

EffectPlan planEffects(const Settings& settings, const EffectConditions& conditions)
{
    auto copy = settings; // describe() takes settings it may change
    EffectSwitches described;
    describe(copy, described);
    const auto on = [&](Effect effect) { return *described.switches[effect]; };
    const auto asksForNeeds = [&](Effect effect) {
        const auto& feature = featureOf(effect);
        return (!feature.needs || on(*feature.needs)) && (!feature.needsRayReconstruction || settings.upscaling.isRayReconstructionAsked());
    };
    const auto replacerOf = [&](Effect effect) {
        std::optional<Effect> replacer;
        for (auto next = featureOf(effect).replacedBy; next; next = featureOf(*next).replacedBy)
            if (on(*next) && asksForNeeds(*next)) replacer = next;

        return replacer;
    };

    EffectPlan plan;
    for (const auto effect : AllEffects)
    {
        const auto& feature = featureOf(effect);
        plan.replacers[effect] = replacerOf(effect);
        plan.states[effect] = !on(effect)                               ? EffectState::Off
            : feature.needsSupportedBuild && !conditions.supportedBuild ? EffectState::Unsupported
            : !conditions.enhancements                                  ? EffectState::Held
            : plan.replacers[effect]                                    ? EffectState::Replaced
            : conditions.leftOut.effects[effect]                        ? EffectState::LeftOut
                                                                        : EffectState::Running;
    }

    // Once every effect has its state: what it builds on may come later in the order.
    for (const auto effect : AllEffects)
    {
        const auto needs = featureOf(effect).needs;
        if (plan.runs(effect) && (!asksForNeeds(effect) || (needs && !plan.runs(*needs)))) plan.states[effect] = EffectState::Lacking;
    }

    return plan;
}

Configuration::Configuration(const Settings& settings, const Diagnostics& diagnostics, const EffectConditions& conditions)
    : settings(settings), diagnostics(diagnostics), leftOut(conditions.leftOut), supportedBuild(conditions.supportedBuild), areEnhancementsOn(conditions.enhancements),
      effects(planEffects(settings, conditions))
{
}

Settings loadSettings(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input)
    {
        if (!std::filesystem::exists(path)) return firstLaunchSettings();

        throw std::runtime_error("Cannot read the Phantom FX settings.");
    }

    IniValues values;
    std::string line;
    while (std::getline(input, line))
    {
        const auto separator = line.find('=');
        if (separator == line.npos) continue;

        values.insert_or_assign(std::string(trim(std::string_view(line).substr(0, separator))), std::string(trim(std::string_view(line).substr(separator + 1))));
    }

    if (input.bad()) throw std::runtime_error("Cannot read the Phantom FX settings.");

    Settings settings;
    IniReader reader(values);
    describe(settings, reader);

    return settings;
}

void saveSettings(const std::filesystem::path& path, const Settings& settings)
{
    if (!inRange(settings)) throw std::runtime_error("Cannot save invalid Phantom FX settings.");

    auto temporary = path;
    temporary += L".tmp";
    try
    {
        std::ofstream output(temporary, std::ios::trunc);
        output.imbue(std::locale::classic());
        writeDescribed(output, settings);
        output.close();
        if (!output) throw std::runtime_error("Cannot write the Phantom FX settings.");

        // Keep the last complete file if writing or replacing it fails.
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot replace the Phantom FX settings");
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
}
