// DLSS Ray Reconstruction on Direct3D 12 for the Direct3D 11 game; ray_reconstruction.hpp describes the route.
#include "features/upscaling/ray_reconstruction.hpp"

#include "features/frame_generation/frame_generation.hpp"
#include "features/upscaling/d3d12_host.hpp"
#include "hresult.hpp"
#include "log.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace phantom_fx::ray_reconstruction
{
namespace
{
// What the feature was created for.
struct FeatureKey
{
    UINT renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    int quality{};
    upscaler::ReconstructionPreset preset{};
    bool hdr{};

    bool operator==(const FeatureKey&) const = default;
};

struct State
{
    explicit State(ID3D11Device* device) : host(device, "Ray Reconstruction") {}

    d3d12_host::Host host;

    bool initialized{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    FeatureKey key;
    OpenedTexture color, depth, motion, output, normalRoughness, diffuseAlbedo, specularAlbedo, dofGuide, specularHitDistance;
};
std::unique_ptr<State> state;

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::string text(message ? message : "");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    log("NGX (Direct3D 12): " + text);
}

void releaseFeature(State& s)
{
    if (!s.feature) return;

    // The feature's resources may still be in use by the last evaluation.
    s.host.waitForLast();
    NVSDK_NGX_D3D12_ReleaseFeature(s.feature);
    s.feature = nullptr;
}

// Throws with a message for the player when Ray Reconstruction cannot run here.
void initializeNgx(State& s, const std::filesystem::path& directory)
{
    const auto modelDirectory = directory.wstring();
    const wchar_t* paths[]{modelDirectory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    auto result = NVSDK_NGX_D3D12_Init_with_ProjectID(frame_generation::NgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Fox Engine 1.0.15",
        directory.c_str(), s.host.device(), &info);
    if (NVSDK_NGX_FAILED(result)) throw std::runtime_error("NVIDIA NGX could not start on Direct3D 12 (" + hex(static_cast<std::uint32_t>(result)) + ").");

    s.initialized = true;
    int available{};
    result = NVSDK_NGX_D3D12_GetCapabilityParameters(&s.parameters);
    if (NVSDK_NGX_SUCCEED(result)) NVSDK_NGX_Parameter_GetI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
    if (!available)
    {
        int needsDriver{};
        unsigned major{}, minor{};
        if (s.parameters)
        {
            NVSDK_NGX_Parameter_GetI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsDriver);
            NVSDK_NGX_Parameter_GetUI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &major);
            NVSDK_NGX_Parameter_GetUI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &minor);
        }

        throw std::runtime_error(needsDriver ? "Ray Reconstruction needs NVIDIA driver " + std::to_string(major) + "." + std::to_string(minor) + " or newer."
                                             : std::string("Ray Reconstruction is unavailable on this GPU. nvngx_dlssd.dll must be beside the add-on."));
    }
}

// The feature keeps its history at the sizes it was created for. Records its creation into `list`;
// returns true when it made one.
bool ensureFeature(State& s, ID3D12GraphicsCommandList* list, const FeatureKey& key)
{
    if (s.feature && s.key == key) return false;

    releaseFeature(s);
    for (const char* hint : {NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(s.parameters, hint, static_cast<unsigned>(key.preset));

    NVSDK_NGX_DLSSD_Create_Params create{};
    create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
    create.InWidth = key.renderWidth;
    create.InHeight = key.renderHeight;
    create.InTargetWidth = key.outputWidth;
    create.InTargetHeight = key.outputHeight;
    create.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(key.quality);
    // The motion vectors are per render pixel; reversed-Z depth; linear light.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (key.hdr) create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    const auto result = NGX_D3D12_CREATE_DLSSD_EXT(list, 1, 1, &s.feature, s.parameters, &create);
    if (NVSDK_NGX_FAILED(result))
    {
        s.feature = nullptr;
        throw std::runtime_error("Creating the Ray Reconstruction feature failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
    }

    s.key = key;
    log("Created DLSS Ray Reconstruction on Direct3D 12: " + std::to_string(key.renderWidth) + "x" + std::to_string(key.renderHeight) + " -> "
        + std::to_string(key.outputWidth) + "x" + std::to_string(key.outputHeight) + ", preset " + std::string(upscaler::label(key.preset)) + ".");

    return true;
}

}

void start(ID3D11Device* device, const std::filesystem::path& directory)
{
    if (state) return;

    auto s = std::make_unique<State>(device);
    try
    {
        initializeNgx(*s, directory);
    }
    catch (...)
    {
        if (s->parameters) NVSDK_NGX_D3D12_DestroyParameters(s->parameters);
        if (s->initialized) NVSDK_NGX_D3D12_Shutdown1(s->host.device());
        throw;
    }

    state = std::move(s);
    log("Ray Reconstruction started on Direct3D 12.");
}

bool started() noexcept
{
    return state != nullptr;
}

bool evaluate(const Inputs& inputs)
{
    auto& s = *state;
    D3D11_TEXTURE2D_DESC color{}, output{};
    inputs.color->GetDesc(&color);
    inputs.output->GetDesc(&output);

    bool created{};
    s.host.evaluate([&](d3d12_host::Evaluation& evaluation) {
        auto* list = evaluation.list();
        created = ensureFeature(s, list, {color.Width, color.Height, output.Width, output.Height, inputs.quality, inputs.preset, inputs.hdr});

        // NGX keeps the matrices' pointers only for the call.
        auto worldToView = inputs.worldToView;
        auto viewToClip = inputs.viewToClip;
        NVSDK_NGX_D3D12_DLSSD_Eval_Params dlssd{};
        dlssd.pInColor = evaluation.read(s.color, inputs.color, "Opening the Ray Reconstruction colour");
        dlssd.pInDepth = evaluation.read(s.depth, inputs.depth, "Opening the Ray Reconstruction depth");
        dlssd.pInMotionVectors = evaluation.read(s.motion, inputs.motion, "Opening the Ray Reconstruction motion");
        dlssd.pInNormals = evaluation.read(s.normalRoughness, inputs.normalRoughness, "Opening the Ray Reconstruction normals");
        dlssd.pInDiffuseAlbedo = evaluation.read(s.diffuseAlbedo, inputs.diffuseAlbedo, "Opening the Ray Reconstruction diffuse albedo");
        dlssd.pInSpecularAlbedo = evaluation.read(s.specularAlbedo, inputs.specularAlbedo, "Opening the Ray Reconstruction specular albedo");
        if (inputs.dofGuide) dlssd.pInDepthOfFieldGuide = evaluation.read(s.dofGuide, inputs.dofGuide, "Opening the Ray Reconstruction depth of field guide");
        dlssd.pInOutput = evaluation.write(s.output, inputs.output, "Opening the Ray Reconstruction output");
        if (inputs.specularHitDistance)
            dlssd.pInSpecularHitDistance = evaluation.read(s.specularHitDistance, inputs.specularHitDistance, "Opening the Ray Reconstruction specular hit distance");

        dlssd.InJitterOffsetX = inputs.jitter[0];
        dlssd.InJitterOffsetY = inputs.jitter[1];
        dlssd.InRenderSubrectDimensions = {color.Width, color.Height};
        dlssd.InReset = inputs.reset || created ? 1 : 0;
        dlssd.InMVScaleX = 1;
        dlssd.InMVScaleY = 1;
        dlssd.pInWorldToViewMatrix = worldToView.data();
        dlssd.pInViewToClipMatrix = viewToClip.data();
        dlssd.InFrameTimeDeltaInMsec = inputs.frameTime;
        const auto result = NGX_D3D12_EVALUATE_DLSSD_EXT(list, s.feature, s.parameters, &dlssd);
        if (NVSDK_NGX_FAILED(result)) throw std::runtime_error("Ray Reconstruction failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
    });

    return created;
}

void release() noexcept
{
    if (!state || !state->feature) return;

    try
    {
        releaseFeature(*state);
        log("Released DLSS Ray Reconstruction.");
    }
    catch (const std::exception& error)
    {
        log(error.what());
    }
}

void stop() noexcept
{
    if (!state) return;

    auto& s = *state;
    release();

    if (s.parameters) NVSDK_NGX_D3D12_DestroyParameters(s.parameters);
    if (s.initialized) NVSDK_NGX_D3D12_Shutdown1(s.host.device());
    state.reset();
    log("Ray Reconstruction stopped.");
}
}
