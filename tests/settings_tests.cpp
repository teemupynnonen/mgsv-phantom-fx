#include "phantom_fx_settings.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// PhantomFX.ini against the settings it describes: every key saved and read back, unknown keys ignored,
// a missing file a first launch, and invalid settings never written. A setting that a feature forgets
// to describe fails the first check.
namespace
{
using namespace phantom_fx;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path settingsPath()
{
    return std::filesystem::temp_directory_path() / "PhantomFX-settings-tests.ini";
}

// Every setting moved off its default, so a key that is not saved, or not read back, shows in the
// comparison. Both switch overloads exist: a feature without a switch describes itself once.
class Extremes final : public SettingVisitor
{
public:
    void feature(bool& on, const FeatureInfo&) override { on = !on; }
    void toggle(bool& value, const ToggleInfo&) override { value = !value; }

    void slider(float& value, const SliderInfo& info) override { value = info.max; }

    void choice(int& value, const ChoiceInfo& info) override
    {
        if (info.choices.size() < 2) return;

        value = value == info.choices.front().value ? info.choices.back().value : info.choices.front().value;
    }
};

void everySettingSurvivesASaveAndLoad()
{
    Settings saved = firstLaunchSettings();
    Extremes extremes;
    describe(saved, extremes);

    const auto path = settingsPath();
    saveSettings(path, saved);
    const Settings loaded = loadSettings(path);
    std::filesystem::remove(path);

    expect(loaded == saved, "Every setting comes back as it was saved.");
}

void unknownKeysAreIgnored()
{
    const auto path = settingsPath();
    const Settings settings = firstLaunchSettings();
    saveSettings(path, settings);
    {
        std::ofstream append(path, std::ios::app);
        append << "NotAKey=42\n";
    }

    const Settings loaded = loadSettings(path);
    std::filesystem::remove(path);

    expect(loaded == settings, "A key no feature describes changes nothing.");
}

void aMissingFileIsAFirstLaunch()
{
    const auto path = std::filesystem::temp_directory_path() / "PhantomFX-settings-tests-missing.ini";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    expect(loadSettings(path) == firstLaunchSettings(), "Without a file, the first launch's settings.");
}

void invalidSettingsAreNotSaved()
{
    class Outside final : public SettingVisitor
    {
    public:
        void slider(float& value, const SliderInfo& info) override
        {
            if (done) return;

            value = info.max + 1;
            done = true;
        }

    private:
        bool done{};
    };

    Settings settings;
    Outside outside;
    describe(settings, outside);

    bool refused{};
    try
    {
        saveSettings(settingsPath(), settings);
    }
    catch (const std::exception&)
    {
        refused = true;
    }

    expect(refused, "Settings outside their range are not written.");
}
}

int main()
{
    try
    {
        everySettingSurvivesASaveAndLoad();
        unknownKeysAreIgnored();
        aMissingFileIsAFirstLaunch();
        invalidSettingsAreNotSaved();
        std::cout << "Settings tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
