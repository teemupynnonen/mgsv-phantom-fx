#include "features/hdr/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr Hdr Defaults{};
}

void describe(Hdr& hdr, SettingVisitor& visit)
{
    visit.feature(hdr.on,
        {.menuId = "hdr",
            .iniKey = "hdr",
            .section = Section::Image,
            .name = "HDR output",
            .tech = "HDR10",
            .tip = "The game in HDR10 on an HDR display: the sun, lamps, fire and reflections shine brighter than the game's white. It needs HDR "
                   "on for the display in Windows. Turning it on or off takes a restart; the brightness applies at once.",
            .summary = "Brighter sun, lamps and fire. Turn HDR on in Windows first.",
            .keywords = "peak brightness paper white nits"});
    visit.slider(hdr.peakNits,
        {.iniKey = "hdr_peak_nits",
            .label = "Peak brightness",
            .min = 400,
            .max = 10000,
            .format = "%.0f nits",
            .logarithmic = true,
            .tip = "The brightest your display shows; by default, what it reports. The brightest the game gets lands on it.",
            .automaticHint = "What this display reports."});
    visit.slider(hdr.paperWhiteNits,
        {.iniKey = "hdr_paper_white_nits",
            .label = "Paper white",
            .min = 80,
            .max = 500,
            .standard = Defaults.paperWhiteNits,
            .format = "%.0f nits",
            .tip = "How bright the game's white is, the HUD and menus included. 203 nits is the HDR reference; raise it in a bright room."});
}
}
