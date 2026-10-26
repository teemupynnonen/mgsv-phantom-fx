#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace phantom_fx::upscaler
{
// NVIDIA DLSS, or AMD FSR: FSR 4 on Radeon RX 7000 and 9000, FSR 3.1 on other GPUs.
enum class Upscaler : std::uint8_t { Dlss, Fsr, Count };
// DLAA replaces the game's FXAA at native resolution (FSR's Native AA); the other modes render below
// the output size and upscale. Off leaves the game as it is.
enum class Mode : std::uint8_t { Off, Dlaa, Quality, Balanced, Performance, UltraPerformance, Custom, Count };
// Values match NVSDK_NGX_DLSS_Hint_Render_Preset; Default lets the DLSS DLL choose.
enum class Preset : std::uint8_t { Default = 0, J = 10, K = 11, L = 12, M = 13 };
// The volumetric fog's look: the game's own sky and fog colours, or Hillaire's physical sky and air.
enum class AtmosphereLook : std::uint8_t { Mgsv, Physical, Count };
// How far the world is drawn and loaded: the game's own, or full detail and the world's coarser copies
// loaded farther (Far loads the coarser copies farther still), with more detail and longer draw
// distances (draw_distance.hpp).
enum class DrawDistance : std::uint8_t { Vanilla, Extended, Far, Count };
// Frames presented per rendered frame, by DLSS Frame Generation up to Dynamic, which chooses them to
// reach the display's refresh rate. Fsr is FSR frame generation, 2x on any GPU.
enum class FrameGeneration : std::uint8_t { Off, X2, X3, X4, X5, X6, Dynamic, Fsr, Count };
// The effects switched together: the screen-space ones, or the ray-traced ones in their place.
enum class EffectPreset : std::uint8_t { ScreenSpace, RayTraced, Count };

struct Settings
{
    static constexpr float MinScale = 0.33f, MaxScale = 1.0f, DefaultCustomScale = 0.75f;
    static constexpr float MinMipBias = -2.0f, MaxMipBias = 1.0f;
    static constexpr float DefaultSharpness = 0.33f, MaxSharpness = 1.0f;
    static constexpr float MinOcclusionRadius = 0.25f, MaxOcclusionRadius = 4.0f, DefaultOcclusionRadius = 1.0f;
    static constexpr float MaxOcclusionStrength = 3.0f;
    static constexpr float MinIndirectLightRadius = 0.25f, MaxIndirectLightRadius = 4.0f, DefaultIndirectLightRadius = 2.0f;
    static constexpr float MaxIndirectLightStrength = 3.0f;
    static constexpr float MaxGlobalIlluminationStrength = 3.0f;
    static constexpr float MinSunSize = 0.1f, MaxSunSize = 4.0f, DefaultSunSize = 0.53f;
    static constexpr float MinFNumber = 0.7f, MaxFNumber = 32.0f, DefaultFNumber = 1.4f;
    static constexpr int DefaultRayQuality = 4;
    static constexpr int DefaultGlobalIlluminationRays = 2;
    static constexpr float MinFlashlightBrightness = 1.0f, MaxFlashlightBrightness = 16.0f, DefaultFlashlightBrightness = 8.0f;
    static constexpr float MinPeakNits = 400, MaxPeakNits = 10000;
    static constexpr float MinPaperWhiteNits = 80, MaxPaperWhiteNits = 500, DefaultPaperWhiteNits = 203;

    Upscaler upscaler{Upscaler::Dlss};
    Mode mode{Mode::Dlaa};
    Preset preset{Preset::Default};
    // RCAS strength on the upscaled output; 0 leaves it unsharpened.
    float sharpness{DefaultSharpness};
    // Render width divided by output width in Custom mode.
    float customScale{DefaultCustomScale};
    // Added to the automatic log2(render / output) texture LOD bias.
    float mipBiasOffset{};

    // Lifts the game's 60 fps cap (frame_rate.hpp), whatever the mode.
    bool unlockFps{};
    // Turning it on, or switching between DLSS and FSR, takes a restart: the game then presents
    // through Direct3D 12 (presenter.hpp).
    FrameGeneration frameGeneration{FrameGeneration::Off};
    // Contact-hardening soft shadows from the sun's shadow map (shadows.hpp), whatever the mode.
    // The sun size is its angular diameter in degrees; the real sun's is 0.53.
    bool softShadows{};
    float sunSize{DefaultSunSize};
    // Bend Studio's screen-space shadows on the sun and on every point and spot light (shadows.hpp),
    // whatever the mode. Apart because the lights' cost grows with how many are on screen.
    bool sunScreenSpaceShadows{};
    bool lightScreenSpaceShadows{};
    // The sun's ray-traced shadows through the global illumination's scene (shadows.hpp), whatever
    // the mode; soft shadows and sun contact shadows are off while it is on. Uses the sun size. With
    // vanilla shadows the game's shadow map adds its shadow near the camera: the darker of the two.
    bool rayTracedShadows{};
    bool vanillaShadowsWithRayTracing{};
    // The point and spot lights drawn again with specular and a ray-traced shadow through the same scene
    // (local_lights.hpp), whatever the mode; lamp contact shadows are off while it is on. The ray quality
    // is their shadow rays a pixel: 1, 2, 4 or 8. The flashlights' brightness multiplies the game's; their
    // reach grows with its square root. 8x is about a real 1980s weapon light against the game's night.
    bool rayTracedLights{};
    int rayQuality{DefaultRayQuality};
    float flashlightBrightness{DefaultFlashlightBrightness};
    // GTVBAO on the indirect light in place of the game's SSAO (indirect_light.hpp), whatever the mode.
    // The radius is in metres; the strength is an exponent on the visibility.
    bool ambientOcclusion{};
    float occlusionRadius{DefaultOcclusionRadius};
    float occlusionStrength{1};
    // GT-VBGI's screen-space indirect lighting (indirect_light.hpp), whatever the mode. The radius is
    // in metres; the strength multiplies the light.
    bool indirectLight{};
    float indirectLightRadius{DefaultIndirectLightRadius};
    float indirectLightStrength{1};
    // Ray-traced global illumination after AMD's GI-1.2 in place of the game's probe light and
    // SSAO (global_illumination.hpp), whatever the mode; GTVBAO and GT-VBGI are off while it is on.
    // The strength multiplies its light. The ray quality is its rays a 2 x 2 of pixels: 1, 2 or 4.
    bool globalIllumination{};
    float globalIlluminationStrength{1};
    int globalIlluminationRays{DefaultGlobalIlluminationRays};
    // Volumetric fog with light shafts in place of the game's fog (atmosphere.hpp), whatever the mode,
    // in the game's look or the physical one, which also draws the sky. The physical look's haze runs
    // from 0 (clear air) to 1 (as thick as the game's own haze).
    bool volumetricFog{};
    AtmosphereLook atmosphereLook{AtmosphereLook::Mgsv};
    float atmosphereHaze{};
    // Whatever the mode. The loading distance takes effect when a location loads, the rest at once.
    DrawDistance drawDistance{DrawDistance::Vanilla};
    // Photo mode's aperture, as an f-number.
    float fNumber{DefaultFNumber};
    // HDR10 output (hdr.hpp). Turning it on or off takes a restart: the swap chain and the scene targets
    // are made for it. The display's peak brightness (0: as the display reports it) and the brightness
    // of the game's white, in nits, apply at once.
    bool hdr{};
    float peakNits{};
    float paperWhiteNits{DefaultPaperWhiteNits};

    bool operator==(const Settings&) const = default;
};

std::string_view label(Upscaler upscaler);
std::string_view label(Mode mode, Upscaler upscaler);
std::string_view label(Preset preset);
std::string_view label(FrameGeneration mode);
std::string_view label(AtmosphereLook look);
std::string_view label(DrawDistance distance);
// 2 to 6 for the fixed DLSS modes, 0 for Off, Dynamic and FSR.
unsigned multiplier(FrameGeneration mode);
bool validPreset(int value);
bool validRayQuality(int value);
bool validGlobalIlluminationRays(int value);
// Quality, Balanced, Performance, Ultra Performance and Custom render below the output size.
bool upscales(Mode mode);

// Render width divided by output width, by NVIDIA's ratios or AMD's. DLSS's own optimal sizes take
// precedence when NGX is ready.
float nominalScale(Upscaler upscaler, Mode mode, float customScale);
// Texture LOD bias that keeps output-resolution texture detail when rendering below it.
float mipBias(float scale, float offset);

// Switches the preset's effects on and the others off. Both presets also unlock the frame rate and turn
// on the volumetric fog in the physical look. The upscaler, HDR and the effects' own settings (radius,
// strength, ray quality) stay as they are.
void applyPreset(Settings& settings, EffectPreset preset);
// The preset the effects are switched as, if any.
std::optional<EffectPreset> presetOf(const Settings& settings);
// A first launch, without a settings file: the screen-space preset, upscaled at Quality.
Settings firstLaunchSettings();

// A missing file gives the first launch's settings; invalid fields keep their defaults; I/O failures throw.
Settings loadSettings(const std::filesystem::path& path);
void saveSettings(const std::filesystem::path& path, const Settings& settings);
}
