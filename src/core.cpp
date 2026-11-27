#include "core.hpp"

#include "features/hdr/hdr.hpp"
#include "features/upscaling/upscaler.hpp"
#include "log.hpp"
#include "dev_tools.hpp"
#include "modules.hpp"

#include <Windows.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace phantom_fx::core
{
namespace
{
using Microsoft::WRL::ComPtr;

std::filesystem::path settingsPath;
bool isSupported{};
bool isFirstLaunch{}; // No settings file yet: the upscaler follows the GPU once attach sees it
std::mutex settingsMutex;
Settings settings;
Diagnostics diagnostics;
bool enhancements{!ToolsOnly}; // Off turns the lighting enhancements off whatever their settings say
LeftOut leftOut;         // A cost measurement's step, over the settings
std::optional<std::uint64_t> unsavedChange; // GetTickCount64 of the latest change PhantomFX.ini does not have yet

// Call with settingsMutex held. Each module takes what it needs, so every change goes to them all.
void configureModules()
{
    const Configuration configuration(settings, diagnostics, {.supportedBuild = isSupported, .enhancements = enhancements, .leftOut = leftOut});
    upscaler::configure(configuration);
    modules::configure(configuration);
    hdr::configure(configuration);
}

// An unreadable adapter counts as NVIDIA's, which keeps the default.
bool isNvidiaGPU(ID3D11Device* device)
{
    constexpr UINT Nvidia = 0x10DE;

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&description)))
        return true;

    return description.VendorId == Nvidia;
}

void savePendingSettings(std::uint64_t settle)
{
    // The tools build forces some settings off to keep the frame as it is: those are not the user's values to keep.
    if constexpr (ToolsOnly) return;
    Settings unsaved;
    {
        std::lock_guard lock(settingsMutex);
        if (!unsavedChange || GetTickCount64() - *unsavedChange < settle) return;

        unsaved = settings;
        unsavedChange.reset();
    }

    try
    {
        saveSettings(settingsPath, unsaved);
    }
    catch (const std::exception& error)
    {
        log(std::string("Could not save the Phantom FX settings: ") + error.what());
    }
}
}

namespace
{
// The tools build leaves the game's frame as it is: upscaling and frame generation would change it whatever the
// effects' settings say.
Settings unchanged(Settings wanted)
{
    if constexpr (ToolsOnly)
    {
        wanted.upscaling.mode = upscaler::Mode::Off;
        wanted.upscaling.rayReconstruction = false;
        wanted.frameGeneration.mode = upscaler::FrameGenerationMode::Off;
    }

    return wanted;
}
}

void initialize(const std::filesystem::path& directory, bool supportedBuild) noexcept
{
    settingsPath = directory / "PhantomFX.ini";
    isSupported = supportedBuild;
    std::error_code ignored;
    isFirstLaunch = !std::filesystem::exists(settingsPath, ignored);

    std::lock_guard lock(settingsMutex);
    try
    {
        settings = loadSettings(settingsPath);
    }
    catch (const std::exception& error)
    {
        log(std::string("Could not load the Phantom FX settings: ") + error.what());
    }

    // Whatever came, the tools build takes none of it into the frame.
    settings = unchanged(settings);

    configureModules();
    log(std::string(upscaler::label(settings.upscaling.upscaler)) + " mode " + std::string(upscaler::label(settings.upscaling.mode, settings.upscaling.upscaler))
        + (isSupported ? "." : "; this game build is not supported, so it stays off."));
}

void attach(ID3D11Device* device) noexcept
{
    // DLSS runs only on NVIDIA's GPUs; FSR on any.
    if (!std::exchange(isFirstLaunch, false) || isNvidiaGPU(device)) return;

    std::lock_guard lock(settingsMutex);
    settings.upscaling.upscaler = upscaler::Upscaler::Fsr;
    configureModules();
    log("First start on a GPU other than NVIDIA's: FSR in place of DLSS.");
}

void detach() noexcept
{
    savePendingSettings(0);
}

void present() noexcept
{
    savePendingSettings(500);
}

Settings currentSettings()
{
    std::lock_guard lock(settingsMutex);
    return settings;
}

void applySettings(const Settings& wanted)
{
    std::lock_guard lock(settingsMutex);
    settings = unchanged(wanted);
    unsavedChange = GetTickCount64();
    configureModules();
}

Diagnostics currentDiagnostics()
{
    std::lock_guard lock(settingsMutex);
    return diagnostics;
}

void applyDiagnostics(const Diagnostics& options)
{
    std::lock_guard lock(settingsMutex);
    diagnostics = options;
    configureModules();
}

bool enhancementsOn()
{
    std::lock_guard lock(settingsMutex);
    return enhancements;
}

void setEnhancements(bool on)
{
    if constexpr (ToolsOnly)
    {
        static bool said{};
        if (on && !said)
        {
            said = true;
            log("The tools build leaves the game's frame as it is: the enhancements stay off.");
        }

        on = false;
    }

    {
        std::lock_guard lock(settingsMutex);
        enhancements = on;
        configureModules();
    }

    log(on ? "Visual enhancements on." : "Visual enhancements off.");
}

void toggleEnhancements()
{
    setEnhancements(!enhancementsOn());
}

EffectPlan effectPlan(const Settings& wanted)
{
    std::lock_guard lock(settingsMutex);
    return planEffects(wanted, {.supportedBuild = isSupported, .enhancements = enhancements});
}

void leaveOut(const LeftOut& effects)
{
    std::lock_guard lock(settingsMutex);
    leftOut = effects;
    configureModules();
}
}
