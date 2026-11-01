#include "features/indirect_light/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr IndirectLight Defaults{};

const SettingChoice Resolutions[]{{0, "Full"}, {1, "Half"}, {2, "Quarter"}};
const SettingChoice DebugViews[]{{0, "Off"}, {1, "GI only"}, {2, "History delta"}, {3, "History length"}, {4, "Reprojection"}, {5, "Variance"}};
}

void describe(IndirectLight& light, SettingVisitor& visit)
{
    visit.feature(light.on,
        {.menuId = "indirectLight",
            .iniKey = "indirect_light",
            .section = Section::Lighting,
            .name = "Indirect lighting",
            .tech = "VB-SSGI",
            .tip = "Screen-space GI with a visibility bitmask: light bounces off nearby surfaces onto others, taking their colour, "
                   "such as sunlit ground brightening the underside of a jeep. Each gather sample is ray-marched against the "
                   "prefiltered depth, so occluders stop the light instead of bleeding through, and it accumulates over frames and "
                   "denoises. It only knows what is on screen. Off leaves the game's indirect light unchanged.",
            .summary = "Light bounces off nearby surfaces and takes their colour; occluders stop it. It only knows what is on screen.",
            .keywords = "VB-SSGI SSGI bounce gather visibility occlusion radius strength samples history denoise",
            .effect = Effect::IndirectLight,
            .replacedBy = Effect::GlobalIllumination,
            .needsSupportedBuild = true});
    visit.slider(light.radius,
        {.iniKey = "indirect_light_radius",
            .label = "Radius",
            .min = 0.05f,
            .max = 1,
            .standard = Defaults.radius,
            .format = "%.0f %%",
            .readoutScale = 100,
            .tip = "How far around a surface the gather looks for surfaces that light it, as a share of the screen's height."});
    visit.slider(light.strength,
        {.iniKey = "indirect_light_strength",
            .label = "Strength",
            .max = 3,
            .standard = Defaults.strength,
            .tip = "1 is one full bounce; higher brightens the bounced light, 0 turns it off."});
    visit.slider(light.samples,
        {.iniKey = "indirect_light_samples",
            .label = "Samples",
            .min = 2,
            .max = 16,
            .standard = Defaults.samples,
            .format = "%.0f",
            .tip = "Gather samples per pixel. More is less noise, at linear cost."});
    visit.choice(light.resolution,
        {.iniKey = "indirect_light_resolution",
            .label = "Resolution",
            .choices = Resolutions,
            .tip = "The resolution the gather, its history and its denoiser run at."});
    visit.slider(light.denoisePasses,
        {.iniKey = "indirect_light_denoise",
            .label = "Denoise passes",
            .min = 1,
            .max = 3,
            .standard = Defaults.denoisePasses,
            .format = "%.0f",
            .tip = "A-trous iterations the denoiser runs; each widens its reach four times."});
    visit.slider(light.history,
        {.iniKey = "indirect_light_history",
            .label = "History",
            .max = 0.95f,
            .standard = Defaults.history,
            .format = "%.0f %%",
            .readoutScale = 100,
            .tip = "Ceiling on how much of the previous frame survives: the bounce accumulates over frames up to this, which steadies it."});
    visit.slider(light.visibility,
        {.iniKey = "indirect_light_visibility",
            .label = "Visibility",
            .max = 8,
            .standard = Defaults.visibility,
            .format = "%.0f",
            .tip = "Ray-march steps per gather tap over the prefiltered depth, testing what blocks it: occluders stop the light "
                   "instead of bleeding through. 0 turns the occlusion test off."});
    visit.choice(light.debug,
        {.iniKey = "indirect_light_debug",
            .label = "Debug view",
            .choices = DebugViews,
            .control = ChoiceControl::Dropdown,
            .tip = "Shows a diagnostic view instead of the scene, the gathered light alone (before the game multiplies it by the "
                   "albedo)."});
}
}  // namespace phantom_fx
