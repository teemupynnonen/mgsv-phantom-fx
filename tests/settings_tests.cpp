#include "phantom_fx_settings.hpp"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// PhantomFX.ini against the settings it describes: every key describe() visits saved and read back,
// unknown keys ignored, a missing file a first launch, and invalid settings never written. A setting
// describe() omits is outside these tests.
namespace
{
using namespace phantom_fx;
namespace fs = std::filesystem;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// One directory per process, so two test runs at once do not share a fixture.
fs::path settingsRoot()
{
    return fs::temp_directory_path() / (L"phantom-fx-settings-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
}

fs::path settingsPath(const fs::path& root)
{
    return root / L"PhantomFX.ini";
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

void everySettingSurvivesASaveAndLoad(const fs::path& root)
{
    Settings saved = firstLaunchSettings();
    Extremes extremes;
    describe(saved, extremes);

    const auto path = settingsPath(root);
    saveSettings(path, saved);
    const Settings loaded = loadSettings(path);

    expect(loaded == saved, "Every setting comes back as it was saved.");
}

void unknownKeysAreIgnored(const fs::path& root)
{
    const auto path = settingsPath(root);
    const Settings settings = firstLaunchSettings();
    saveSettings(path, settings);
    {
        std::ofstream append(path, std::ios::app);
        append << "NotAKey=42\n";
    }

    const Settings loaded = loadSettings(path);

    expect(loaded == settings, "A key no feature describes changes nothing.");
}

void aMissingFileIsAFirstLaunch(const fs::path& root)
{
    expect(loadSettings(root / L"missing.ini") == firstLaunchSettings(), "Without a file, the first launch's settings.");
}

void invalidSettingsAreNotSaved(const fs::path& root)
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
        saveSettings(settingsPath(root), settings);
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
    const auto root = settingsRoot();
    try
    {
        fs::create_directories(root);
        everySettingSurvivesASaveAndLoad(root);
        unknownKeysAreIgnored(root);
        aMissingFileIsAFirstLaunch(root);
        invalidSettingsAreNotSaved(root);
        fs::remove_all(root);
        std::cout << "Settings tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);

        return 1;
    }
}
