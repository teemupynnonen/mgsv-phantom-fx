#include "features/upscaling/settings.hpp"

#include <algorithm>
#include <cmath>

namespace phantom_fx
{
namespace
{
using upscaler::Mode;
using upscaler::Preset;
using upscaler::ReconstructionPreset;
using upscaler::Upscaler;

constexpr Upscaling Defaults{};

constexpr SettingChoice Upscalers[]{{static_cast<int>(Upscaler::Dlss), "DLSS", "dlss"}, {static_cast<int>(Upscaler::Fsr), "FSR", "fsr"}};

constexpr SettingChoice modeChoice(Mode mode, const char* label, std::string_view iniKey)
{
    return {static_cast<int>(mode), label, iniKey};
}

constexpr SettingChoice DlssModes[]{
    modeChoice(Mode::Off, "Off (game FXAA)", "off"),
    modeChoice(Mode::Dlaa, "DLAA", "dlaa"),
    modeChoice(Mode::Quality, "Quality", "quality"),
    modeChoice(Mode::Balanced, "Balanced", "balanced"),
    modeChoice(Mode::Performance, "Performance", "performance"),
    modeChoice(Mode::UltraPerformance, "Ultra Performance", "ultra_performance"),
    modeChoice(Mode::Custom, "Custom", "custom"),
};
// FSR calls its native mode Native AA.
constexpr SettingChoice FsrModes[]{
    modeChoice(Mode::Off, "Off (game FXAA)", "off"),
    modeChoice(Mode::Dlaa, "Native AA", "dlaa"),
    modeChoice(Mode::Quality, "Quality", "quality"),
    modeChoice(Mode::Balanced, "Balanced", "balanced"),
    modeChoice(Mode::Performance, "Performance", "performance"),
    modeChoice(Mode::UltraPerformance, "Ultra Performance", "ultra_performance"),
    modeChoice(Mode::Custom, "Custom", "custom"),
};

// In the menu's order.
constexpr SettingChoice Presets[]{
    {static_cast<int>(Preset::Default), "Default"},
    {static_cast<int>(Preset::K), "K (transformer)"},
    {static_cast<int>(Preset::J), "J (transformer, less ghosting)"},
    {static_cast<int>(Preset::M), "M (Performance model)"},
    {static_cast<int>(Preset::L), "L (Ultra Performance model)"},
};

// In the menu's order.
constexpr SettingChoice ReconstructionPresets[]{
    {static_cast<int>(ReconstructionPreset::Default), "Default"},
    {static_cast<int>(ReconstructionPreset::F), "F (Ray Reconstruction 2)"},
    {static_cast<int>(ReconstructionPreset::E), "E (transformer)"},
    {static_cast<int>(ReconstructionPreset::D), "D (older transformer)"},
};

std::span<const SettingChoice> modesFor(Upscaler upscaler)
{
    return upscaler == Upscaler::Fsr ? std::span<const SettingChoice>(FsrModes) : std::span<const SettingChoice>(DlssModes);
}

template<class E> std::string_view labelIn(std::span<const SettingChoice> choices, E value)
{
    const auto found = std::ranges::find(choices, static_cast<int>(value), &SettingChoice::value);
    return found == choices.end() ? "Unknown" : found->label;
}
}

void describe(Upscaling& upscaling, SettingVisitor& visit)
{
    visit.feature({.menuId = "upscaler",
        .section = Section::Image,
        .name = "Upscaler",
        .tip = "NVIDIA DLSS needs an NVIDIA RTX GPU. AMD FSR runs on any GPU: FSR 4 on Radeon RX 7000 and 9000, FSR 3.1 on the rest.",
        .summary = "Replaces the game's FXAA anti-aliasing.",
        .keywords = "DLSS FSR DLAA native AA mode quality balanced performance preset ray reconstruction sharpening texture detail bias render scale"});
    visit.choice(upscaling.upscaler, {.iniKey = "upscaler", .label = "Upscaler", .choices = Upscalers});
    visit.choice(upscaling.mode,
        {.iniKey = "mode",
            .label = "Mode",
            .choices = modesFor(upscaling.upscaler),
            .control = ChoiceControl::Dropdown,
            .tip = "DLAA and Native AA replace the game's FXAA at full resolution. Quality to Ultra Performance render the 3D scene below the "
                   "output resolution and upscale it; the HUD and menus stay at full resolution. Off leaves the game unchanged.",
            .isItemValue = true});
    visit.slider(upscaling.customRenderScale,
        {.iniKey = "custom_scale",
            .label = "Render scale",
            .min = Upscaling::MinRenderScale,
            .max = Upscaling::MaxRenderScale,
            .standard = Defaults.customRenderScale,
            .format = "%.0f%%",
            .readoutScale = 100,
            .onRelease = true,
            .tip = "Render width as a share of the output width. Applies when released.",
            .visible = upscaling.mode == Mode::Custom});

    const bool on = upscaling.mode != Mode::Off;
    visit.choice(upscaling.dlssPreset,
        {.iniKey = "preset",
            .label = "DLSS preset",
            .choices = Presets,
            .control = ChoiceControl::Dropdown,
            .tip = "Default lets DLSS choose (K for DLAA).",
            .visible = upscaling.upscaler == Upscaler::Dlss,
            .enabled = on});
    visit.toggle(upscaling.rayReconstruction,
        {.iniKey = "ray_reconstruction",
            .label = "Ray Reconstruction",
            .tip = "DLSS Ray Reconstruction in place of DLSS: one NVIDIA model that cleans up the ray-traced light and upscales. It runs while a "
                   "ray-traced effect is on (bounce light, shadows or lights) and needs an NVIDIA RTX GPU. Ray traced: Ultra needs it.",
            .visible = upscaling.upscaler == Upscaler::Dlss,
            .enabled = on,
            .isRaySetting = true});
    visit.choice(upscaling.reconstructionPreset,
        {.iniKey = "ray_reconstruction_preset",
            .label = "Reconstruction preset",
            .choices = ReconstructionPresets,
            .control = ChoiceControl::Dropdown,
            .tip = "Default lets Ray Reconstruction choose (F). D takes no depth of field guide, so noise can show through the game's depth of field.",
            .visible = upscaling.upscaler == Upscaler::Dlss && upscaling.rayReconstruction,
            .enabled = on,
            .isRaySetting = true});
    visit.slider(upscaling.sharpness,
        {.iniKey = "sharpness",
            .label = "Sharpening",
            .standard = Defaults.sharpness,
            .format = "%.0f%%",
            .readoutScale = 100,
            .tip = "AMD FidelityFX RCAS on the upscaled image. 0% is off.",
            .enabled = on});
    visit.slider(upscaling.textureBiasOffset,
        {.iniKey = "mip_bias_offset",
            .label = "Texture detail bias",
            .min = Upscaling::MinTextureBiasOffset,
            .max = Upscaling::MaxTextureBiasOffset,
            .standard = Defaults.textureBiasOffset,
            .format = "%+.2f",
            .onRelease = true,
            .tip = "Added to the automatic texture LOD bias for the render size. Lower is sharper but can shimmer. Applies when released.",
            .hint = "Lower is sharper, but can shimmer.",
            .enabled = on});
}

namespace upscaler
{
std::string_view label(Upscaler upscaler)
{
    return labelIn(Upscalers, upscaler);
}

std::string_view label(Mode mode, Upscaler upscaler)
{
    return labelIn(modesFor(upscaler), mode);
}

std::string_view label(Preset preset)
{
    return labelIn(Presets, preset);
}

std::string_view label(ReconstructionPreset preset)
{
    return labelIn(ReconstructionPresets, preset);
}

bool upscales(Mode mode)
{
    return mode >= Mode::Quality && mode <= Mode::Custom;
}

float nominalScale(Upscaler upscaler, Mode mode, float customScale)
{
    switch (mode)
    {
    case Mode::Quality: return 2.0f / 3.0f;
    // AMD's Balanced divides by 1.7.
    case Mode::Balanced: return upscaler == Upscaler::Fsr ? 1.0f / 1.7f : 0.58f;
    case Mode::Performance: return 0.5f;
    case Mode::UltraPerformance: return 1.0f / 3.0f;
    case Mode::Custom:
        return std::isfinite(customScale) ? std::clamp(customScale, Upscaling::MinRenderScale, Upscaling::MaxRenderScale) : Defaults.customRenderScale;
    default: return 1.0f;
    }
}

float mipBias(float scale, float offset)
{
    if (!std::isfinite(scale) || scale <= 0) scale = 1;
    if (!std::isfinite(offset)) offset = 0;

    return std::log2(std::min(scale, 1.0f)) + std::clamp(offset, Upscaling::MinTextureBiasOffset, Upscaling::MaxTextureBiasOffset);
}
}
}
