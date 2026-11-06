#include "features/ray_traced_lights/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr RayTracedLights Defaults{};
constexpr SettingChoice Qualities[]{{1, "Low 1"}, {2, "Medium 2"}, {4, "High 4"}, {8, "Ultra 8"}};
// Ray traced: Ultra (path tracing) lights the flashlights too.
constexpr Effect FlashlightsShownIn[]{Effect::RayTracedLights, Effect::PathTracing};
}

void describe(RayTracedLights& lights, SettingVisitor& visit)
{
    visit.feature(lights.on,
        {.menuId = "rayTracedLights",
            .iniKey = "ray_traced_lights",
            .section = Section::Lighting,
            .name = "Lamp shadows",
            .tech = "RT",
            .tag = "Experimental",
            .tip = "Every lamp, spot light and flashlight casts shadows from rays traced through the game's own scene, in place of the game's "
                   "shadow maps or none at all: most of the game's lights, the flashlights among them, light through walls and characters. "
                   "Lights without specular get the game's own. With bounce light on, their light bounces too. Needs a GPU that traces "
                   "rays (DXR 1.1).",
            .summary = "Lamps and flashlights cast real shadows and stop at walls.",
            .keywords = "RT DXR ray-traced lights lamps ray quality flashlight brightness",
            .effect = Effect::RayTracedLights,
            .replacedBy = Effect::PathTracing,
            .needsSupportedBuild = true});
    visit.choice(lights.shadowRaysPerPixel,
        {.iniKey = "ray_quality",
            .label = "Ray quality",
            .choices = Qualities,
            .tip = "Shadow rays a pixel for the lights' soft shadows, shared over the lights that reach it. More rays leave less noise for the "
                   "filter to smooth; the cost grows with them where lamps light the screen."});
    visit.slider(lights.flashlightBrightness,
        {.iniKey = "flashlight_brightness",
            .label = "Flashlight brightness",
            .min = 1,
            .max = 16,
            .standard = Defaults.flashlightBrightness,
            .format = "%.1fx",
            .logarithmic = true,
            .tip = "The weapon flashlights, yours and the soldiers', brighter than the game's; they reach further with it, as a brighter light "
                   "fades to the same dimness further away. 1x is the game's: about 5 times the moonlight at 10 m and cut off at 30 m. 8x is "
                   "about a real 1980s weapon light against the game's night. Only the look: the guards see as before.",
            .shownIn = FlashlightsShownIn});
}
}
