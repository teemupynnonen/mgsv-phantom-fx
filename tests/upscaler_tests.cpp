#include "camera.hpp"
#include "gi_lighting.hpp"
#include "range_allocator.hpp"
#include "shader_bytecode.hpp"
#include "upscaler_settings.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace
{
namespace fs = std::filesystem;
using namespace phantom_fx;
using namespace phantom_fx::upscaler;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool close(double actual, double expected, double tolerance = 1e-4)
{
    return std::abs(actual - expected) <= tolerance;
}

void write(const fs::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary);
    output << text;
    output.close();
    expect(static_cast<bool>(output), "Write the test fixture.");
}

// Over a valid saved file, so a rejected save can be seen to leave it as it was.
bool saveRejected(const fs::path& path, const Settings& saved, const Settings& invalid)
{
    saveSettings(path, saved);
    try
    {
        saveSettings(path, invalid);
    }
    catch (const std::exception&)
    {
        return loadSettings(path) == saved;
    }

    return false;
}

void settingsPersist(const fs::path& root)
{
    const auto path = root / L"PhantomFX.ini";
    const auto first = loadSettings(path);
    expect(first == firstLaunchSettings() && first.upscaler == Upscaler::Dlss && first.mode == Mode::Quality
            && presetOf(first) == EffectPreset::ScreenSpace,
        "A first launch upscales with DLSS Quality and the screen-space preset.");
    expect(!fs::exists(path), "Loading defaults must not create a settings file.");

    const Settings chosen{.upscaler = Upscaler::Fsr, .mode = Mode::Off, .preset = Preset::J, .sharpness = 0.5f, .unlockFps = true, .frameGeneration = FrameGeneration::X3,
        .softShadows = true, .sunSize = 1.25f, .sunScreenSpaceShadows = true, .lightScreenSpaceShadows = true, .rayTracedShadows = true,
        .vanillaShadowsWithRayTracing = true, .rayTracedLights = true, .rayQuality = 8, .flashlightBrightness = 3.5f, .ambientOcclusion = true, .occlusionRadius = 2, .occlusionStrength = 1.5f, .indirectLight = true, .indirectLightRadius = 3,
        .indirectLightStrength = 0.5f, .globalIlluminationRays = 4, .volumetricFog = true, .atmosphereLook = AtmosphereLook::Physical, .atmosphereHaze = 0.5f,
        .drawDistance = DrawDistance::Far};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen,
        "Restart restores the upscaler, mode, preset, sharpening, frame rate unlock, frame generation, soft, screen-space and ray-traced "
        "shadows, GTVBAO, GT-VBGI, the global illumination's rays, the volumetric fog and the draw distance.");
    write(path, "frame_generation=dynamic\n");
    expect(loadSettings(path).frameGeneration == FrameGeneration::Dynamic, "Dynamic frame generation is read back.");
    write(path, "frame_generation=fsr\n");
    expect(loadSettings(path).frameGeneration == FrameGeneration::Fsr, "FSR frame generation is read back.");
    write(path, "frame_generation=8x\n");
    expect(loadSettings(path).frameGeneration == FrameGeneration::Off, "An unknown multiplier leaves frame generation off.");

    write(path, "# DLSS\r\nunknown=future\r\n mode = dlaa \r\npreset=13\r\nbroken\r\n");
    expect(loadSettings(path) == Settings{.mode = Mode::Dlaa, .preset = Preset::M}, "CRLF, whitespace and unknown keys keep the valid fields.");
    write(path, "upscaler=xess\nmode=ultra\npreset=7\nsharpness=nan\ncustom_scale=inf\n");
    expect(loadSettings(path) == Settings{}, "Unknown upscalers and modes, reserved presets and invalid numbers fall back to defaults.");
    write(path, "sharpness=-1\n");
    expect(loadSettings(path).sharpness == 0, "Negative sharpening turns it off.");
    write(path, "sharpness=7\n");
    expect(loadSettings(path).sharpness == Settings::MaxSharpness, "Sharpening is clamped.");
    write(path, "custom_scale=0.1\nmip_bias_offset=-5\n");
    expect(loadSettings(path).customScale == Settings::MinScale && loadSettings(path).mipBiasOffset == Settings::MinMipBias,
        "The custom scale and the texture bias offset are clamped.");
    write(path, "ambient_occlusion=gtvbao\nocclusion_radius=10\nocclusion_strength=-1\n");
    const auto occlusion = loadSettings(path);
    expect(!occlusion.ambientOcclusion && occlusion.occlusionRadius == Settings::MaxOcclusionRadius && occlusion.occlusionStrength == 0,
        "An invalid toggle leaves GTVBAO off, and its radius and strength are clamped.");
    write(path, "soft_shadows=on\nsun_size=20\n");
    const auto shadows = loadSettings(path);
    expect(!shadows.softShadows && shadows.sunSize == Settings::MaxSunSize, "An invalid toggle leaves soft shadows off, and the sun size is clamped.");
    write(path, "indirect_light=yes\nindirect_light_radius=0\nindirect_light_strength=9\n");
    const auto light = loadSettings(path);
    expect(!light.indirectLight && light.indirectLightRadius == Settings::MinIndirectLightRadius
            && light.indirectLightStrength == Settings::MaxIndirectLightStrength,
        "An invalid toggle leaves GT-VBGI off, and its radius and strength are clamped.");

    const Settings upscaling{.mode = Mode::Custom, .preset = Preset::K, .sharpness = 0.25f, .customScale = 0.75f, .mipBiasOffset = -0.5f};
    saveSettings(path, upscaling);
    expect(loadSettings(path) == upscaling, "Restart restores an upscaling mode with its custom scale and texture bias offset.");

    for (const auto& invalid : {Settings{.upscaler = Upscaler::Count}, Settings{.mode = Mode::Count}, Settings{.preset = static_cast<Preset>(7)},
             Settings{.sharpness = std::numeric_limits<float>::quiet_NaN()}, Settings{.sharpness = 2}, Settings{.customScale = 2},
             Settings{.mipBiasOffset = 3}, Settings{.frameGeneration = FrameGeneration::Count}, Settings{.occlusionRadius = 0},
             Settings{.sunSize = 0}, Settings{.occlusionStrength = 4}, Settings{.indirectLightRadius = 5}, Settings{.indirectLightStrength = -1}})
        expect(saveRejected(path, chosen, invalid), "Invalid settings cannot replace valid saved ones.");
}

void effectPresets()
{
    Settings settings{.mode = Mode::Performance, .hdr = true};
    settings.softShadows = true;
    settings.globalIlluminationRays = 4;
    applyPreset(settings, EffectPreset::RayTraced);
    expect(presetOf(settings) == EffectPreset::RayTraced && !settings.softShadows && settings.volumetricFog
            && settings.atmosphereLook == AtmosphereLook::Physical && settings.unlockFps,
        "The ray-traced preset switches the screen-space effects off, the fog on.");
    expect(settings.mode == Mode::Performance && settings.hdr && settings.globalIlluminationRays == 4,
        "A preset keeps the upscaler, HDR and the effects' own settings.");

    settings.indirectLight = true;
    expect(!presetOf(settings), "One effect switched otherwise matches no preset.");
}

// The main view of a recorded frame (docs/DEVELOPMENT.md, "Constant buffers"): P00 = -1.75, P11 = 28/9,
// reversed-Z with near 0.069 and far 4000, at 1920x1080.
constexpr float A = -1.72503e-05f, B = 0.0690012f;
constexpr float ViewRows[4][4]{
    {-0.310798f, 0.0f, 0.950476f, -202.916f},
    {0.0333242f, 0.999385f, 0.0108968f, -319.845f},
    {-0.949891f, 0.0350606f, -0.310607f, 153.871f},
    {0, 0, 0, 1},
};

void fill(camera::SceneConstants& scene, const float (&view)[4][4], const float eye[3])
{
    scene = {};
    const float projection[4][4]{{-1.75f, 0, 0, 0}, {0, 28.0f / 9, 0, 0}, {0, 0, A, B}, {0, 0, 1, 0}};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            scene.c[4 + row][column] = projection[row][column];
            scene.c[8 + row][column] = view[row][column];
            for (int k = 0; k < 4; ++k) scene.c[row][column] += projection[row][k] * view[k][column];
        }

    for (int i = 0; i < 3; ++i) scene.c[20][i] = eye[i];
    scene.c[20][3] = 1;
    scene.c[22][0] = 1920;
    scene.c[22][1] = 1080;
    scene.c[22][2] = 0.069f;
    scene.c[22][3] = 4000;
}

// Pixel position (x right, y down) of a world point through the scene's projection-view.
std::array<double, 2> project(const camera::SceneConstants& scene, const double (&point)[3])
{
    double clip[4]{};
    for (int row = 0; row < 4; ++row)
        clip[row] = scene.c[row][0] * point[0] + scene.c[row][1] * point[1] + scene.c[row][2] * point[2] + scene.c[row][3];

    return {(clip[0] / clip[3] + 1) * 960, (1 - clip[1] / clip[3]) * 540};
}

void mainViewRecognised()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    camera::SceneConstants scene;
    fill(scene, ViewRows, eye);
    expect(camera::classify(scene) == camera::SceneView::MainCamera, "The main view's upload is the camera.");

    auto motionBlur = scene;
    motionBlur.c[8][3] += 5; // MotionBlurCameraVelocity keeps the previous projection-view in c8-c11
    expect(camera::classify(motionBlur) == camera::SceneView::MainProjection, "Passes with other matrices in c8-c11 still get the jitter.");

    auto shadow = scene;
    shadow.c[7][2] = 0;
    shadow.c[7][3] = 1;
    expect(camera::classify(shadow) == camera::SceneView::Other, "Orthographic shadow views are left alone.");

    auto square = scene;
    square.c[4][0] = -square.c[5][1];
    expect(camera::classify(square) == camera::SceneView::Other, "A square perspective view is not the main camera.");

    auto jittered = scene;
    camera::applyJitter(jittered, {0.001f, 0.001f});
    expect(camera::classify(jittered) == camera::SceneView::Other, "An upload is never jittered twice.");
}

void jitterShiftsTheImage()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    camera::SceneConstants scene;
    fill(scene, ViewRows, eye);
    const auto view = camera::viewOf(scene);
    const std::array<float, 2> pixels{0.25f, -0.375f};
    auto jittered = scene;
    camera::applyJitter(jittered, camera::jitterNdc(pixels, 1920, 1080));

    // A point 30 m in front of the camera.
    const double point[3]{eye[0] + 30.0 * ViewRows[2][0], eye[1] + 30.0 * ViewRows[2][1], eye[2] + 30.0 * ViewRows[2][2]};
    const auto before = project(scene, point), after = project(jittered, point);
    expect(close(after[0] - before[0], pixels[0], 1e-3) && close(after[1] - before[1], pixels[1], 1e-3),
        "The image moves by the jitter in render pixels, x right and y down, as DLSS takes it.");
    expect(jittered.c[28][0] == camera::jitterNdc(pixels, 1920, 1080)[0] && jittered.c[28][1] == camera::jitterNdc(pixels, 1920, 1080)[1],
        "m_cameraCenterOffset carries the NDC offset the screen-space passes subtract.");
    expect(camera::viewOf(scene).projectionView == view.projectionView, "The camera is captured before the jitter.");
}

void reprojection()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    camera::SceneConstants scene;
    fill(scene, ViewRows, eye);
    const auto view = camera::viewOf(scene);
    const auto still = camera::previousFromCurrent(view, view);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            expect(close(still[row * 4 + column], row == column ? 1 : 0, 1e-9), "A camera that stands still has no motion.");

    // Previous frame: the camera 0.2 m to the side, so a point straight ahead was elsewhere on screen.
    camera::SceneConstants before;
    const float previousEye[3]{eye[0] - 0.2f * ViewRows[0][0], eye[1] - 0.2f * ViewRows[0][1], eye[2] - 0.2f * ViewRows[0][2]};
    float previousRows[4][4];
    std::memcpy(previousRows, ViewRows, sizeof(previousRows));
    for (int row = 0; row < 3; ++row)
        previousRows[row][3] = -(ViewRows[row][0] * previousEye[0] + ViewRows[row][1] * previousEye[1] + ViewRows[row][2] * previousEye[2]);

    fill(before, previousRows, previousEye);
    const auto moved = camera::previousFromCurrent(camera::viewOf(before), view);

    const double point[3]{eye[0] + 10.0 * ViewRows[2][0], eye[1] + 10.0 * ViewRows[2][1], eye[2] + 10.0 * ViewRows[2][2]};
    const auto now = project(scene, point), then = project(before, point);
    double clip[4]{};
    for (int row = 0; row < 4; ++row)
        clip[row] = scene.c[row][0] * point[0] + scene.c[row][1] * point[1] + scene.c[row][2] * point[2] + scene.c[row][3];

    const double position[4]{clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3], 1};
    double previous[4]{};
    for (int row = 0; row < 4; ++row)
        for (int k = 0; k < 4; ++k) previous[row] += moved[row * 4 + k] * position[k];

    const double x = (previous[0] / previous[3] + 1) * 960, y = (1 - previous[1] / previous[3]) * 540;
    expect(close(x, then[0], 0.05) && close(y, then[1], 0.05) && !close(now[0], then[0], 1),
        "Depth and the two matrices find where a point was in the previous frame.");
}

void cuts()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    camera::SceneConstants scene;
    fill(scene, ViewRows, eye);
    const auto view = camera::viewOf(scene);

    auto walked = view;
    walked.eye[0] += 0.3f;
    expect(!camera::cameraCut(view, walked), "Riding or running is not a cut.");

    auto jumped = view;
    jumped.eye[1] += 3;
    expect(camera::cameraCut(view, jumped), "A camera that jumps metres in one frame is a cut.");

    auto turned = view;
    turned.forward = {-view.forward[0], view.forward[1], -view.forward[2]};
    expect(camera::cameraCut(view, turned), "A reverse shot is a cut.");

    auto zoomed = view;
    zoomed.focal *= 1.5f;
    expect(camera::cameraCut(view, zoomed), "A sudden change of field of view is a cut.");
}

// DLSS Frame Generation reprojects with these, so they must describe the camera the engine draws with.
void streamlineCamera()
{
    // The eye the view's translation puts the camera at; the recorded one is rounded by millimetres.
    float eye[3]{};
    for (int i = 0; i < 3; ++i)
        for (int row = 0; row < 3; ++row) eye[i] -= ViewRows[row][i] * ViewRows[row][3];

    camera::SceneConstants scene;
    fill(scene, ViewRows, eye);
    const auto view = camera::viewOf(scene);
    const auto still = camera::streamlineCamera(view, view);

    // A point ahead, to the side and above: into Streamline's view space along the camera's axes from
    // its position, then v x viewToClip, lands on the pixel the engine draws it on.
    double point[3]{};
    for (int i = 0; i < 3; ++i) point[i] = eye[i] + 10.0 * ViewRows[2][i] + 2.0 * ViewRows[0][i] + 1.0 * ViewRows[1][i];
    double local[4]{0, 0, 0, 1};
    for (int i = 0; i < 3; ++i)
    {
        const double offset = point[i] - still.position[i];
        local[0] += offset * still.right[i];
        local[1] += offset * still.up[i];
        local[2] += offset * still.forward[i];
    }

    double clip[4]{};
    for (int column = 0; column < 4; ++column)
        for (int k = 0; k < 4; ++k) clip[column] += local[k] * still.viewToClip[k * 4 + column];

    const auto engine = project(scene, point);
    expect(close((clip[0] / clip[3] + 1) * 960, engine[0], 0.05) && close((1 - clip[1] / clip[3]) * 540, engine[1], 0.05),
        "Streamline's camera puts a point on the pixel the engine draws it on.");
    expect(engine[0] < 960 && local[0] < 0 && engine[1] < 540 && local[1] > 0,
        "A point the engine draws left of and above the centre is left of and above it in Streamline's view space.");
    expect(still.viewToClip[0] > 0 && still.viewToClip[5] > 0 && still.viewToClip[11] == 1,
        "The projection is the usual one: x right, y up, w = z.");

    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            double product = 0;
            for (int k = 0; k < 4; ++k) product += still.clipToView[row * 4 + k] * still.viewToClip[k * 4 + column];
            expect(close(product, row == column ? 1 : 0, 1e-4), "clipToView inverts viewToClip.");
            expect(close(still.clipToPreviousClip[row * 4 + column], row == column ? 1 : 0, 1e-6)
                    && close(still.previousClipToClip[row * 4 + column], row == column ? 1 : 0, 1e-6),
                "A camera that stands still maps clip space onto itself.");
        }

    expect(close(still.verticalFov, 2 * std::atan(9.0 / 28)) && close(still.aspectRatio, 16.0 / 9)
            && still.nearPlane == 0.069f && still.farPlane == 4000,
        "The field of view, aspect ratio and planes are the engine's.");
}

void modeMaths()
{
    expect(!upscales(Mode::Off) && !upscales(Mode::Dlaa) && upscales(Mode::Quality) && upscales(Mode::Custom),
        "Only the modes below native resolution upscale.");
    constexpr auto Dlss = Upscaler::Dlss, Fsr = Upscaler::Fsr;
    expect(close(nominalScale(Dlss, Mode::Quality, 0), 2.0 / 3) && close(nominalScale(Dlss, Mode::Balanced, 0), 0.58)
            && close(nominalScale(Dlss, Mode::Performance, 0), 0.5) && close(nominalScale(Dlss, Mode::UltraPerformance, 0), 1.0 / 3)
            && nominalScale(Dlss, Mode::Dlaa, 0.5f) == 1,
        "DLSS's modes keep NVIDIA's render scales.");
    expect(close(nominalScale(Fsr, Mode::Quality, 0), 1 / 1.5) && close(nominalScale(Fsr, Mode::Balanced, 0), 1 / 1.7)
            && close(nominalScale(Fsr, Mode::Performance, 0), 0.5) && close(nominalScale(Fsr, Mode::UltraPerformance, 0), 1.0 / 3)
            && nominalScale(Fsr, Mode::Dlaa, 0.5f) == 1,
        "FSR's modes keep AMD's render scales.");
    expect(nominalScale(Dlss, Mode::Custom, 0.8f) == 0.8f && nominalScale(Fsr, Mode::Custom, 0.1f) == Settings::MinScale
            && nominalScale(Dlss, Mode::Custom, std::numeric_limits<float>::quiet_NaN()) == Settings::DefaultCustomScale,
        "Custom uses its scale within the supported range.");

    expect(mipBias(1, 0) == 0 && close(mipBias(0.5f, 0), -1) && close(mipBias(2.0f / 3, 0), std::log2(2.0 / 3)),
        "The texture LOD bias is log2(render / output).");
    expect(close(mipBias(0.5f, 0.25f), -0.75) && close(mipBias(0.5f, -9), -1 + Settings::MinMipBias) && mipBias(0, 0) == 0,
        "The offset is added within its range, and invalid scales give no bias.");

    expect(multiplier(FrameGeneration::Off) == 0 && multiplier(FrameGeneration::X2) == 2 && multiplier(FrameGeneration::X6) == 6
            && multiplier(FrameGeneration::Dynamic) == 0 && multiplier(FrameGeneration::Fsr) == 0,
        "Fixed DLSS frame generation modes present 2 to 6 frames per rendered frame.");
}

void jitterSequence()
{
    expect(camera::jitterPhases(1.0f) == 8, "DLAA uses 8 phases.");
    expect(camera::jitterPhases(0.5f) == 32 && camera::jitterPhases(1.0f / 3) == 72 && camera::jitterPhases(0) == 8,
        "Lower render scales get NVIDIA's recommended count; an invalid scale gets 8.");

    std::set<std::pair<float, float>> offsets;
    double sumX = 0, sumY = 0;
    for (std::uint64_t frame = 0; frame < 8; ++frame)
    {
        const auto jitter = camera::jitterPixels(frame, 8);
        expect(jitter[0] >= -0.5f && jitter[0] < 0.5f && jitter[1] >= -0.5f && jitter[1] < 0.5f, "Jitter stays inside the pixel.");
        offsets.emplace(jitter[0], jitter[1]);
        sumX += jitter[0];
        sumY += jitter[1];
    }

    expect(offsets.size() == 8, "Each of the 8 phases samples a different position.");
    expect(std::abs(sumX / 8) < 0.07 && std::abs(sumY / 8) < 0.07, "The phases are centred on the pixel.");
    expect(camera::jitterPixels(3, 8) == camera::jitterPixels(11, 8), "The sequence repeats after its phases.");
    expect(close(camera::jitterPixels(0, 8)[0], 0) && close(camera::jitterPixels(0, 8)[1], 1.0 / 3 - 0.5), "It is Halton(2, 3) from index 1.");
}

void photoModeSettingsPersist(const fs::path& root)
{
    const auto path = root / L"PhantomFX-photo-mode.ini";
    const Settings chosen{.fNumber = 2.8f};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen, "Restart restores photo mode's f-stop.");

    write(path, "f_number=0.1\n");
    expect(loadSettings(path).fNumber == Settings::MinFNumber, "An f-stop out of range is clamped.");

    expect(saveRejected(path, chosen, Settings{.fNumber = 64}), "An invalid f-stop cannot replace a valid saved one.");
}

void hdrSettingsPersist(const fs::path& root)
{
    const auto path = root / L"PhantomFX-hdr.ini";
    const Settings chosen{.hdr = true, .peakNits = 1500, .paperWhiteNits = 250};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen, "Restart restores HDR output, the peak brightness and the paper white.");

    write(path, "hdr=yes\nhdr_peak_nits=50000\nhdr_paper_white_nits=10\n");
    const auto read = loadSettings(path);
    expect(!read.hdr && read.peakNits == Settings::MaxPeakNits && read.paperWhiteNits == Settings::MinPaperWhiteNits,
        "An invalid toggle leaves HDR off, and the peak brightness and paper white are clamped.");
    write(path, "hdr_peak_nits=0\n");
    expect(loadSettings(path).peakNits == 0, "A peak brightness of 0 keeps the display's own.");

    for (const auto& invalid : {Settings{.peakNits = 100}, Settings{.peakNits = std::numeric_limits<float>::quiet_NaN()},
             Settings{.paperWhiteNits = std::numeric_limits<float>::infinity()}})
        expect(saveRejected(path, chosen, invalid), "Invalid HDR settings cannot replace valid saved ones.");
}

// HDR clamps the 2D pass's writes by patching its bytecode, which Direct3D takes only with a correct
// checksum. Shaders of different lengths exercise both forms of the checksum's last block.
void outputSaturation()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);
    std::set<std::size_t> lastBlocks;
    std::string body = "float4 c = color;";
    for (int terms = 1; terms <= 24; ++terms)
    {
        body += " c = c * " + std::to_string(terms + 1) + ".5 + " + std::to_string(terms) + ";";
        const auto source = "float4 main(float4 color : COLOR0) : SV_Target { " + body + " return c; }";
        ComPtr<ID3DBlob> code, errors;
        expect(SUCCEEDED(D3DCompile(source.data(), source.size(), "test", nullptr, nullptr, "main", "ps_5_0", 0, 0, code.GetAddressOf(),
                   errors.GetAddressOf())),
            "Compile the test shader.");
        const auto* bytes = code->GetBufferPointer();
        const auto size = code->GetBufferSize();
        expect(dxbcChecksum(bytes, size) == *dxbcHashOf(bytes, size), "The checksum matches the compiler's.");
        lastBlocks.insert((size - 20) % 64 < 56);

        // The first multiply's literal is found among the operands (the HDR tonemap patch finds its
        // literals this way).
        const auto* tokens = static_cast<const std::uint8_t*>(bytes);
        const auto program = instructionsOf(tokens, size);
        bool literalFound{};
        for (const auto& instruction : program.value_or(std::vector<Instruction>{}))
            for (const auto offset : immediatesOf(tokens, instruction))
            {
                float value{};
                std::memcpy(&value, tokens + offset, sizeof(value));
                literalFound = literalFound || value == 2.5f;
            }
        // The compiler folds later terms together, so only the first shader surely keeps 2.5.
        expect(program && (literalFound || terms > 1), "The program's instructions and their literals are read.");

        const auto patched = saturateOutput(bytes, size);
        expect(patched && dxbcChecksum(patched->data(), patched->size()) == *dxbcHashOf(patched->data(), patched->size()),
            "The patched shader carries its new checksum.");
        ComPtr<ID3DBlob> text;
        expect(SUCCEEDED(D3DDisassemble(patched->data(), patched->size(), 0, nullptr, text.GetAddressOf()))
                && std::string_view(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize()).find("_sat o0") != std::string_view::npos,
            "The write to the render target is saturated.");

        ComPtr<ID3D11PixelShader> shader;
        expect(!device || SUCCEEDED(device->CreatePixelShader(patched->data(), patched->size(), nullptr, shader.GetAddressOf())),
            "Direct3D takes the patched shader.");
    }

    expect(lastBlocks.size() == 2, "The shaders end in both forms of the checksum's last block.");
}
}

void globalIlluminationSettingsPersist(const fs::path& root)
{
    const auto path = root / L"PhantomFX-gi.ini";
    const Settings chosen{.globalIllumination = true, .globalIlluminationStrength = 1.5f};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen, "Restart restores the global illumination and its strength.");
    write(path, "global_illumination_strength=99\n");
    expect(loadSettings(path).globalIlluminationStrength == Settings::MaxGlobalIlluminationStrength, "The strength is clamped.");
}

// The scene's geometry and acceleration structures live in large buffers handed out in ranges.
void rangesComeBack()
{
    RangeAllocator allocator(1000);
    const auto a = allocator.allocate(100), b = allocator.allocate(300), c = allocator.allocate(100);
    expect(a == 0u && b == 100u && c == 400u, "Ranges are handed out first fit, one after another.");
    allocator.release(*b, 300);
    expect(allocator.allocate(200) == 100u, "A released range is used again.");
    expect(allocator.allocate(64, 256) == 512u, "An aligned range starts at a multiple of the alignment.");
    expect(!allocator.allocate(1000), "What does not fit is refused.");

    allocator.release(*a, 100);
    allocator.release(100, 200);
    allocator.release(*c, 100);
    allocator.release(512, 64);
    expect(allocator.usedSize() == 0 && allocator.allocate(1000) == 0u, "Released neighbours merge into one range again.");
}

// Traced rays that reach the sky must light a surface nothing blocks as the game's sky light does.
void skyLightMatchesTheGame()
{
    // f(n) = 1 + 0.5 n.y + 0.3 n.y^2 with the game's c = (-n.x, -n.z, n.y, 1): c.z is n.y.
    std::array<std::array<float, 16>, 3> matrices{};
    for (auto& m : matrices)
    {
        m[15] = 1;
        m[11] = m[14] = 0.25f;
        m[10] = 0.3f;
    }

    constexpr float Exposure = 2;
    const auto radiance = gi_lighting::skyRadiance(matrices, Exposure);
    constexpr double Pi = 3.14159265358979323846;
    const std::array<std::array<float, 3>, 4> normals{{{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0.6f, 0.8f, 0}}};
    for (const auto& n : normals)
    {
        // Irradiance over pi from the radiance, by brute force.
        double sum = 0;
        constexpr int Rings = 256, Segments = 512;
        for (int ring = 0; ring < Rings; ++ring)
        {
            const double theta = (ring + 0.5) * Pi / Rings;
            for (int segment = 0; segment < Segments; ++segment)
            {
                const double phi = (segment + 0.5) * 2 * Pi / Segments;
                const std::array<float, 3> w{static_cast<float>(std::sin(theta) * std::cos(phi)), static_cast<float>(std::sin(theta) * std::sin(phi)),
                    static_cast<float>(std::cos(theta))};
                const double cosine = std::max(0.0, double{n[0]} * w[0] + double{n[1]} * w[1] + double{n[2]} * w[2]);
                sum += gi_lighting::evaluate(radiance, w)[0] * cosine * std::sin(theta) * (Pi / Rings) * (2 * Pi / Segments);
            }
        }

        const double game = Exposure * (1 + 0.5 * n[1] + 0.3 * n[1] * n[1]);
        expect(std::abs(sum / Pi - game) < 0.01 * game, "An open surface gets the game's sky light from the traced sky.");
    }
}

int main()
{
    const auto root =
        fs::temp_directory_path() / (L"phantom-fx-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try
    {
        fs::create_directories(root);
        settingsPersist(root);
        effectPresets();
        mainViewRecognised();
        jitterShiftsTheImage();
        reprojection();
        cuts();
        streamlineCamera();
        modeMaths();
        jitterSequence();
        photoModeSettingsPersist(root);
        hdrSettingsPersist(root);
        outputSaturation();
        globalIlluminationSettingsPersist(root);
        rangesComeBack();
        skyLightMatchesTheGame();
        fs::remove_all(root);
        std::cout << "Upscaler tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);

        return 1;
    }
}
