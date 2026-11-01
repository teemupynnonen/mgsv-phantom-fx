#pragma once

#include "effects.hpp"
#include "features/ambient_occlusion/settings.hpp"
#include "features/contact_shadow_shape/settings.hpp"
#include "features/depth_of_field/settings.hpp"
#include "features/frame_generation/settings.hpp"
#include "features/frame_rate/settings.hpp"
#include "features/global_illumination/settings.hpp"
#include "features/hdr/settings.hpp"
#include "features/indirect_light/settings.hpp"
#include "features/lamp_contact_shadows/settings.hpp"
#include "features/lamp_soft_shadows/settings.hpp"
#include "features/motion_blur/settings.hpp"
#include "features/path_tracing/settings.hpp"
#include "features/photo_mode/settings.hpp"
#include "features/ray_traced_lights/settings.hpp"
#include "features/ray_traced_shadows/settings.hpp"
#include "features/sea/settings.hpp"
#include "features/setting_visitor.hpp"
#include "features/soft_shadows/settings.hpp"
#include "features/sun/settings.hpp"
#include "features/sun_contact_shadows/settings.hpp"
#include "features/upscaling/settings.hpp"
#include "features/volumetric_fog/settings.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>

// Phantom FX's settings (PhantomFX.ini): its features' together. Also the diagnostics, the effect plan,
// and what each module configures itself from.
namespace phantom_fx
{
struct Settings
{
    Upscaling upscaling;
    FrameGeneration frameGeneration;
    Sun sun;
    RayTracedShadows rayTracedShadows;
    SoftShadows softShadows;
    SunContactShadows sunContactShadows;
    RayTracedLights rayTracedLights;
    LampContactShadows lampContactShadows;
    LampSoftShadows lampSoftShadows;
    ContactShadowShape contactShadowShape;
    GlobalIllumination globalIllumination;
    PathTracing pathTracing;
    AmbientOcclusion ambientOcclusion;
    IndirectLight indirectLight;
    VolumetricFog volumetricFog;
    Sea sea;
    DepthOfField depthOfField;
    MotionBlur motionBlur;
    PhotoMode photoMode;
    Hdr hdr;
    FrameRate frameRate;

    bool operator==(const Settings&) const = default;
};

// Calls `visit` with each feature's settings, in the menu's order.
template<class Visit> void forEachFeature(Settings& settings, Visit&& visit)
{
    visit(settings.upscaling);
    visit(settings.frameGeneration);
    visit(settings.sun);
    visit(settings.rayTracedShadows);
    visit(settings.rayTracedLights);
    visit(settings.softShadows);
    visit(settings.lampSoftShadows);
    visit(settings.sunContactShadows);
    visit(settings.lampContactShadows);
    visit(settings.contactShadowShape);
    visit(settings.globalIllumination);
    visit(settings.pathTracing);
    visit(settings.ambientOcclusion);
    visit(settings.indirectLight);
    visit(settings.volumetricFog);
    visit(settings.sea);
    visit(settings.depthOfField);
    visit(settings.motionBlur);
    visit(settings.photoMode);
    visit(settings.hdr);
    visit(settings.frameRate);
}

void describe(Settings& settings, SettingVisitor& visit);

// What a feature is: the start of its description.
template<class Feature> FeatureInfo infoOf()
{
    class Start final : public SettingVisitor
    {
    public:
        void feature(bool&, const FeatureInfo& feature) override { info = feature; }

        void feature(const FeatureInfo& feature) override { info = feature; }

        FeatureInfo info{};
    };

    Feature feature;
    Start start;
    describe(feature, start);

    return start.info;
}

// What a lighting effect is, from its feature's description.
const FeatureInfo& featureOf(Effect effect);
bool& switchOf(Settings& settings, Effect effect);

// The effects switched together: the screen-space ones, or the ray-traced ones in their place.
enum class EffectPreset : std::uint8_t { ScreenSpace, RayTraced, Count };

// Switches the preset's effects on and the others off. Both presets also unlock the frame rate and turn
// on the volumetric fog in the physical look. The upscaler, HDR and the effects' own settings (radius,
// strength, ray quality) stay as they are.
void applyPreset(Settings& settings, EffectPreset preset);
// The preset the effects are switched as, if any.
std::optional<EffectPreset> presetOf(const Settings& settings);
// A first launch, without a settings file: the screen-space preset, upscaled at Quality.
Settings firstLaunchSettings();

// What the image shows for troubleshooting. The Shadow views, of the ray-traced shadows, and the Light
// views, of the global illumination's light, blend over the image; the Shadow ones with the shadows'
// probe at the screen's centre.
enum class DebugView : std::uint8_t
{
    FinalImage,
    MotionVectors,
    ObjectMotion,    // Where the engine's velocity was used
    ShadowRays,      // What blocks each pixel's ray to the sun
    ShadowStarts,    // Where each pixel's shadow rays leave
    LightHistory,    // The denoiser's history
    LightValidation, // NRD's validation layer
};

// The diagnostic views and switches. Not saved.
struct Diagnostics
{
    DebugView debugView{};
    bool noJitter{};
    bool gameFxaa{}; // Comparison: leave the frame to the game's FXAA, keeping DLSS ready
    bool onlyScreenSpaceShadows{}; // The sun shadowed by the screen-space shadows alone
    bool showAmbientOcclusion{};   // GTVBAO's occlusion alone in place of the lit scene
    bool showIndirectLight{};      // SSILVB's light alone in place of the lit scene
    bool showGlobalIllumination{}; // The global illumination's light alone in place of the lit scene
    bool showTracedScene{};        // The global illumination's traced scene from the camera
    bool twoSidedShadows{};        // Rays towards lights blocked by every surface from both sides, as until 2026-10-26
    int lightVolumes{};            // 0 the ray-traced lights within the game's volumes, 1 those with a shadow map beyond, 2 every one beyond
    bool logGlobalIlluminationOften{}; // The global illumination's light logged every few frames, not every ten seconds
};

// Effects a cost measurement's step runs without (performance.hpp), while the settings, PhantomFX.ini
// and the overlay keep them on. Leaving out the upscaler shows the game's FXAA at the same render size.
struct LeftOut
{
    ByEffect<bool> effects;
    bool upscaler{}, sharpening{};
    // Part of the global illumination, left out before all of it: its light passes.
    bool globalLight{};
};

// What an effect is: the first of these that applies. Lacking: what it builds on (FeatureInfo::needs,
// needsRayReconstruction) does not run, or is not asked for.
enum class EffectState : std::uint8_t { Off, Unsupported, Held, Replaced, LeftOut, Lacking, Running };

struct EffectPlan
{
    ByEffect<EffectState> states;
    ByEffect<std::optional<Effect>> replacers; // The effect that takes a Replaced one's place

    EffectState operator[](Effect effect) const;
    bool runs(Effect effect) const;
    // A ray-traced effect runs, so the ray-traced scene is needed.
    bool tracesScene() const;
};

// What holds the effects back besides their settings; by default nothing does.
struct EffectConditions
{
    bool supportedBuild{true};
    bool enhancements{true}; // The enhancements switch (F1)
    LeftOut leftOut;
};

// A replaced effect stays off while a replacer's setting is on, with what that one needs, even when the
// replacer is left out or waits. Replacers chain (soft shadows by the ray-traced shadows, those by path
// tracing): the farthest along that is on takes the place.
EffectPlan planEffects(const Settings& settings, const EffectConditions& conditions);

// What each module configures itself from (its configure): core.cpp passes it to them all whenever any
// of it changes, with its lock held, so a module's configure keeps what it needs and calls nothing in
// core.
struct Configuration
{
    // The plan comes from the settings and the conditions, so the two always agree.
    Configuration(const Settings& settings, const Diagnostics& diagnostics, const EffectConditions& conditions);

    const Settings& settings;
    const Diagnostics& diagnostics;
    LeftOut leftOut; // By a cost measurement's step: what the plan does not cover
    bool supportedBuild{};
    EffectPlan effects;
};

// A missing file gives the first launch's settings; invalid fields keep their defaults; I/O failures throw.
Settings loadSettings(const std::filesystem::path& path);
void saveSettings(const std::filesystem::path& path, const Settings& settings);
// `settings` with one line of PhantomFX.ini applied; an unknown key or an invalid value changes nothing.
Settings withLine(Settings settings, std::string_view key, std::string_view value);
}
