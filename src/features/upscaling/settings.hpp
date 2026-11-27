#pragma once

#include "features/setting_visitor.hpp"

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
// Values match NVSDK_NGX_RayReconstruction_Hint_Render_Preset; Default lets the Ray Reconstruction DLL
// choose (F in 310.9.1). D takes no depth of field guide.
enum class ReconstructionPreset : std::uint8_t { Default = 0, D = 4, E = 5, F = 6 };

std::string_view label(Upscaler upscaler);
std::string_view label(Mode mode, Upscaler upscaler);
std::string_view label(Preset preset);
std::string_view label(ReconstructionPreset preset);
// Quality, Balanced, Performance, Ultra Performance and Custom render below the output size.
bool upscales(Mode mode);
// Render width divided by output width, by NVIDIA's ratios or AMD's. DLSS's own optimal sizes take
// precedence when NGX is ready.
float nominalScale(Upscaler upscaler, Mode mode, float customScale);
// Texture LOD bias that keeps output-resolution texture detail when rendering below it: AMD's
// log2(render / output) - 1, as the upscaler resolves several frames of detail back in.
float mipBias(float scale);
}

namespace phantom_fx
{
struct Upscaling
{
    static constexpr float MinRenderScale = 0.33f, MaxRenderScale = 1;

    upscaler::Upscaler upscaler{upscaler::Upscaler::Dlss};
    upscaler::Mode mode{upscaler::Mode::Dlaa};
    upscaler::Preset dlssPreset{upscaler::Preset::Default};
    bool rayReconstruction{};
    upscaler::ReconstructionPreset reconstructionPreset{upscaler::ReconstructionPreset::Default};
    float sharpness{0.33f};
    float customRenderScale{0.75f};

    // DLSS upscales and Ray Reconstruction is asked for; whether it runs is the upscaler's.
    bool isRayReconstructionAsked() const { return upscaler == upscaler::Upscaler::Dlss && mode != upscaler::Mode::Off && rayReconstruction; }

    bool operator==(const Upscaling&) const = default;
};

void describe(Upscaling& upscaling, SettingVisitor& visit);
}
