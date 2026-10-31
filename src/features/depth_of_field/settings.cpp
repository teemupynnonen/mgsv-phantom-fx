#include "features/depth_of_field/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr SettingChoice FarBlurs[]{
    {static_cast<int>(FarBlur::Lens), "Lens", "lens"},
    {static_cast<int>(FarBlur::Mgsv), "MGSV", "mgsv"},
};
}

void describe(DepthOfField& depthOfField, SettingVisitor& visit)
{
    visit.feature({.menuId = "depthOfField",
        .section = Section::Camera,
        .name = "Depth of field",
        .summary = "How the background blurs. It needs Depth of Field on in the game's graphics settings.",
        .keywords = "DOF blur bokeh focus lens background cutscene"});
    visit.choice(depthOfField.farBlur,
        {.iniKey = "depth_of_field_far_blur",
            .label = "Background blur",
            .choices = FarBlurs,
            .tip = "Lens blurs the background as a lens does, each point spread over a smooth disc. MGSV keeps the game's own softer blur. Edges in "
                   "focus stay sharp with either, and what is in front of the focus keeps the game's blur."});
}
}
