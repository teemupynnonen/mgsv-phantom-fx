#include "features/frame_generation/settings.hpp"

#include <algorithm>

namespace phantom_fx
{
namespace
{
using upscaler::FrameGenerationMode;

constexpr SettingChoice modeChoice(FrameGenerationMode mode, const char* label, std::string_view iniKey)
{
    return {static_cast<int>(mode), label, iniKey};
}

constexpr SettingChoice Modes[]{
    modeChoice(FrameGenerationMode::Off, "Off", "off"),
    modeChoice(FrameGenerationMode::X2, "DLSS 2x", "2x"),
    modeChoice(FrameGenerationMode::X3, "DLSS 3x", "3x"),
    modeChoice(FrameGenerationMode::X4, "DLSS 4x", "4x"),
    modeChoice(FrameGenerationMode::X5, "DLSS 5x", "5x"),
    modeChoice(FrameGenerationMode::X6, "DLSS 6x", "6x"),
    modeChoice(FrameGenerationMode::Dynamic, "DLSS Dynamic (display refresh rate)", "dynamic"),
    modeChoice(FrameGenerationMode::Fsr, "FSR 2x", "fsr"),
};
}

void describe(FrameGeneration& generation, SettingVisitor& visit)
{
    visit.feature({.menuId = "frameGeneration",
        .section = Section::Image,
        .name = "Frame generation",
        .tip = "Frames generated between the ones the game renders, with either upscaler. DLSS Frame Generation: 2x needs an RTX 40, 3x to "
               "6x and Dynamic an RTX 50. Dynamic reaches the display's refresh rate. It turns on NVIDIA Reflex. FSR frame generation: 2x on "
               "any GPU, machine-learned on Radeon RX 9000. Turning it on, or switching between DLSS and FSR, takes a restart, as the game "
               "then presents through Direct3D 12; other changes apply at once.",
        .summary = "Extra frames between the real ones. Needs a restart.",
        .keywords = "DLSS FSR Reflex 2x 3x 4x 5x 6x dynamic"});
    visit.choice(generation.mode,
        {.iniKey = "frame_generation", .label = "Frame generation", .choices = Modes, .control = ChoiceControl::Dropdown, .isItemValue = true});
}

namespace upscaler
{
std::string_view label(FrameGenerationMode mode)
{
    const auto found = std::ranges::find(Modes, static_cast<int>(mode), &SettingChoice::value);
    return found == std::end(Modes) ? "Unknown" : found->label;
}

unsigned multiplier(FrameGenerationMode mode)
{
    return mode >= FrameGenerationMode::X2 && mode <= FrameGenerationMode::X6 ? static_cast<unsigned>(mode) + 1 : 0;
}
}
}
