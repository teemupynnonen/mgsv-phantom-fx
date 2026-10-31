cmake_minimum_required(VERSION 3.25)
project(PhantomFX VERSION 1.9.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Phantom FX requires Windows and MSVC.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Phantom FX must be built for 64-bit Windows.")
endif()

# The statistics logged every few seconds, and the measurements behind them, are for development
# (the dev preset); a release logs only what happens once: startup, sizes, failures.
option(PHANTOM_FX_DIAGNOSTIC_LOG "Log the periodic statistics used in development" OFF)
# The overlay's Developer page, tools for testing in the game that players never get (the dev preset).
option(PHANTOM_FX_DEV_TOOLS "Add the Developer page to the overlay" OFF)

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537 # v1.3.4
)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
FetchContent_MakeAvailable(minhook)

# NVIDIA Real-Time Denoisers v4.18.0 for the global illumination (gi_denoiser.cpp): a static library
# with its compute shaders embedded as DXIL, which its build compiles with the Windows SDK's DXC
# through NVIDIA's ShaderMake (both it and MathLib are fetched by NRD's own build).
FetchContent_Declare(nrd
    GIT_REPOSITORY https://github.com/NVIDIA-RTX/NRD.git
    GIT_TAG e06852bd545973cb224eb28820b891205cfc1624 # v4.18.0
)
set(NRD_STATIC_LIBRARY ON)
set(NRD_EMBEDS_DXIL_SHADERS ON)
set(NRD_EMBEDS_DXBC_SHADERS OFF)
set(NRD_EMBEDS_SPIRV_SHADERS OFF)
set(NRD_NORMAL_ENCODING 2) # R10G10B10A2_UNORM, as shaders/gi_light.hlsl packs it
set(NRD_ROUGHNESS_ENCODING 1) # Linear
FetchContent_MakeAvailable(nrd)

# Downloads `source|path|sha256` entries from raw.githubusercontent.com into `directory`.
function(download_pinned directory)
    foreach(entry IN LISTS ARGN)
        string(REPLACE "|" ";" entry "${entry}")
        list(GET entry 0 source)
        list(GET entry 1 path)
        list(GET entry 2 hash)
        set(target "${directory}/${path}")
        if(EXISTS "${target}")
            file(SHA256 "${target}" existing)
            if(existing STREQUAL hash)
                continue()
            endif()
        endif()
        message(STATUS "Downloading ${source}")
        file(DOWNLOAD "https://raw.githubusercontent.com/${source}" "${target}" EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "Could not download ${source}: ${status}")
        endif()
    endforeach()
endfunction()

# NVIDIA DLSS SDK v310.9.1. The repository carries every platform's binaries (over 1 GB),
# so only the D3D11 headers, the static NGX loader and the x64 model DLLs (DLSS and Ray
# Reconstruction) are downloaded.
set(DLSS_SDK_COMMIT 374959484e79a640feaba44c93ac8cfb0a03f5b5)
set(DLSS_SDK_DIR "${CMAKE_BINARY_DIR}/_deps/dlss-sdk")
set(sdk "NVIDIA/DLSS/${DLSS_SDK_COMMIT}")
download_pinned("${DLSS_SDK_DIR}"
    "${sdk}/LICENSE.txt|LICENSE.txt|d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"
    "${sdk}/include/nvsdk_ngx.h|include/nvsdk_ngx.h|dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"
    "${sdk}/include/nvsdk_ngx_defs.h|include/nvsdk_ngx_defs.h|ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"
    "${sdk}/include/nvsdk_ngx_params.h|include/nvsdk_ngx_params.h|943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"
    "${sdk}/include/nvsdk_ngx_helpers.h|include/nvsdk_ngx_helpers.h|5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471"
    "${sdk}/include/nvsdk_ngx_helpers_d3d.h|include/nvsdk_ngx_helpers_d3d.h|ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba"
    "${sdk}/include/nvsdk_ngx_helpers_cuda.h|include/nvsdk_ngx_helpers_cuda.h|f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042"
    "${sdk}/include/nvsdk_ngx_defs_dlssd.h|include/nvsdk_ngx_defs_dlssd.h|2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716"
    "${sdk}/include/nvsdk_ngx_params_dlssd.h|include/nvsdk_ngx_params_dlssd.h|d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e"
    "${sdk}/include/nvsdk_ngx_helpers_dlssd_d3d.h|include/nvsdk_ngx_helpers_dlssd_d3d.h|6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293"
    "${sdk}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib|lib/Windows_x86_64/x64/nvsdk_ngx_s.lib|4e5d355086d2bc11e1a0842457d2519ea528ee1f3e112c45679a84960c07dff3"
    "${sdk}/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib|lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib|f62f94f7a444ddffa118e71bc412298d803784bf042464be178a989915198cab"
    "${sdk}/lib/Windows_x86_64/rel/nvngx_dlss.dll|lib/Windows_x86_64/rel/nvngx_dlss.dll|3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983"
    "${sdk}/lib/Windows_x86_64/rel/nvngx_dlssd.dll|lib/Windows_x86_64/rel/nvngx_dlssd.dll|4bc7ea5fcb2f32cf86bc2cb072e8d2860914a0be511cbdcb2fc206c1d9d83b80"
)
set(DLSS_FILES "${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" "${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")

# ReShade 6.8.0 add-on API headers and the Dear ImGui 1.92.5 header its overlay requires.
set(RESHADE_COMMIT 18deaa52de0c425a78b329e9cb3c497281cd00ec) # v6.8.0
set(IMGUI_COMMIT 3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c) # ReShade's deps/imgui
set(RESHADE_DIR "${CMAKE_BINARY_DIR}/_deps/reshade-api")
set(reshade "crosire/reshade/${RESHADE_COMMIT}")
set(imgui "ocornut/imgui/${IMGUI_COMMIT}")
download_pinned("${RESHADE_DIR}"
    "${reshade}/LICENSE.md|LICENSE.md|237ded5b8344f820113efab1e65e91e1f159d9202c5b4856606a0590d3ffdab0"
    "${reshade}/include/reshade.hpp|include/reshade.hpp|cf3cd150920ea310b4b1e2e4f11ab8d7221a9fc18755f4891a026563311b524c"
    "${reshade}/include/reshade_api.hpp|include/reshade_api.hpp|8d83ec8a620c48da7a20d975bd27b3345b24d3f0d5b78ae4c4fe633d61b804eb"
    "${reshade}/include/reshade_api_device.hpp|include/reshade_api_device.hpp|a716c44b6fbb064dbbb0826687728f21aa71ed45e13564e4dc1cdf36da5bdfa0"
    "${reshade}/include/reshade_api_format.hpp|include/reshade_api_format.hpp|68caa17fef712043e810ab88770c15af009aa3c7693e5c115ddc324f2d0809a7"
    "${reshade}/include/reshade_api_pipeline.hpp|include/reshade_api_pipeline.hpp|5bda8e07260c2c60f0fd18a78b11782aed8f69e5d24b5757874614caafdf416f"
    "${reshade}/include/reshade_api_resource.hpp|include/reshade_api_resource.hpp|32cdafe6d783813f3ac72ad43e426638d8d0fef56bcb558fad218a14cfb75ee3"
    "${reshade}/include/reshade_events.hpp|include/reshade_events.hpp|2717f56e2bd89af6d708c9c316353d6581c5a41d4526629fadba51d13be57a89"
    "${reshade}/include/reshade_overlay.hpp|include/reshade_overlay.hpp|ed914b12fbc41ed038287b0163c0bc934772afcff2c22392044c6008adc5ae79"
    "${imgui}/imgui.h|imgui/imgui.h|07562842049eaca4e47c3fcdcc9d8eafd442695b9856d6f6639e92e1bb6c737e"
    "${imgui}/imconfig.h|imgui/imconfig.h|6e5687893594ebfaf8569280cfea83d025904a8a4e23bec073086f6c5bc8f7fb"
    "${imgui}/LICENSE.txt|imgui/LICENSE.txt|c80c5789748d955c4a650562baa0d750494e2c7128c2ca66aaebe2e3c4e198bf"
)

# NVIDIA Streamline SDK 2.14.1 for DLSS Frame Generation. Only its release archive carries the
# frame generation plugin, so the archive is downloaded once and the headers, the license and the
# x64 runtime files are extracted. The add-on loads Streamline at run time; nothing is linked.
set(STREAMLINE_VERSION 2.14.1)
set(STREAMLINE_DIR "${CMAKE_BINARY_DIR}/_deps/streamline-sdk-${STREAMLINE_VERSION}")
set(STREAMLINE_RUNTIME sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll)
if(NOT EXISTS "${STREAMLINE_DIR}/license.txt")
    set(archive "${CMAKE_BINARY_DIR}/_deps/streamline-sdk-v${STREAMLINE_VERSION}.zip")
    message(STATUS "Downloading the Streamline SDK ${STREAMLINE_VERSION}")
    file(DOWNLOAD "https://github.com/NVIDIA-RTX/Streamline/releases/download/v${STREAMLINE_VERSION}/streamline-sdk-v${STREAMLINE_VERSION}.zip"
        "${archive}" EXPECTED_HASH SHA256=92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Could not download the Streamline SDK: ${status}")
    endif()
    list(TRANSFORM STREAMLINE_RUNTIME PREPEND "bin/x64/" OUTPUT_VARIABLE runtime)
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${STREAMLINE_DIR}" PATTERNS "include/*" license.txt ${runtime})
    file(REMOVE "${archive}")
endif()
list(TRANSFORM STREAMLINE_RUNTIME PREPEND "${STREAMLINE_DIR}/bin/x64/" OUTPUT_VARIABLE STREAMLINE_FILES)

# AMD FSR SDK 2.3.0 for FSR upscaling and frame generation: the API headers and AMD's signed
# Direct3D 12 runtime. The upscaler DLL holds FSR 4 and FSR 3.1 and picks by GPU. The add-on loads
# the runtime at run time; nothing is linked.
set(FIDELITYFX_COMMIT 60f4ea81909200d8542eca14dccb2628b763a9a3) # v2.3.0
set(FIDELITYFX_DIR "${CMAKE_BINARY_DIR}/_deps/fidelityfx-sdk")
set(ffx "GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${FIDELITYFX_COMMIT}/Kits/FidelityFX")
download_pinned("${FIDELITYFX_DIR}"
    "${ffx}/docs/license.md|license.md|6757f2fec461238da6f84f83536ec195e8c036b1773a06f6f34301b9ed8c0963"
    "${ffx}/api/include/ffx_api.h|api/include/ffx_api.h|e1a9d1b559eaf75f4cb401f1c43a66e2f3dc9621abfeb58b9c6fac56443177fd"
    "${ffx}/api/include/ffx_api_types.h|api/include/ffx_api_types.h|cc0adc0ffc804dd7cd03c39c15581b575e416383177ffe95060201d52cc0fc49"
    "${ffx}/api/include/dx12/ffx_api_dx12.h|api/include/dx12/ffx_api_dx12.h|c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530"
    "${ffx}/upscalers/include/ffx_upscale.h|upscalers/include/ffx_upscale.h|65be36d5653eb71c10379e0204f7bb7cd6a8aeec96e6f8cffc5f59ffeb1f89bf"
    "${ffx}/framegeneration/include/ffx_framegeneration.h|framegeneration/include/ffx_framegeneration.h|e6c42f8a38aab2c7c4fc6931545a8e5463fdf87349902ec9687d9fe4949a0566"
    "${ffx}/framegeneration/include/ffx_framegeneration_api_types.h|framegeneration/include/ffx_framegeneration_api_types.h|396596af3df63310674e8036abd2ad0fcd061e61334e7a2c76653a2be3c686b4"
    "${ffx}/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h|framegeneration/include/dx12/ffx_api_framegeneration_dx12.h|d533810bbd6fd4f87dfa2c1657c1457c7b1597b66345827d375b081d67127f66"
    "${ffx}/signedbin/amd_fidelityfx_loader_dx12.dll|bin/amd_fidelityfx_loader_dx12.dll|e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa"
    "${ffx}/signedbin/amd_fidelityfx_upscaler_dx12.dll|bin/amd_fidelityfx_upscaler_dx12.dll|d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46"
    "${ffx}/signedbin/amd_fidelityfx_framegeneration_dx12.dll|bin/amd_fidelityfx_framegeneration_dx12.dll|02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18"
)
set(FIDELITYFX_FILES "${FIDELITYFX_DIR}/bin/amd_fidelityfx_loader_dx12.dll" "${FIDELITYFX_DIR}/bin/amd_fidelityfx_upscaler_dx12.dll"
    "${FIDELITYFX_DIR}/bin/amd_fidelityfx_framegeneration_dx12.dll")

add_library(ngx STATIC IMPORTED)
set_target_properties(ngx PROPERTIES
    IMPORTED_LOCATION "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
    IMPORTED_LOCATION_DEBUG "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${DLSS_SDK_DIR}/include"
)

# ReShade reads the add-on's name, author and version from its version resource.
configure_file(src/version.rc.in "${CMAKE_BINARY_DIR}/version.rc" @ONLY)

# The global illumination's Direct3D 12 kernels (src/shaders) are compiled at build time by the
# Windows SDK's DXC into headers of DXIL, one per entry point; Direct3D 11's shaders compile at run
# time as before.
file(GLOB DXC_SDK_DIRS LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT DXC_SDK_DIRS COMPARE NATURAL ORDER DESCENDING)
find_program(DXC_EXECUTABLE dxc HINTS "$ENV{WindowsSdkVerBinPath}x64" ${DXC_SDK_DIRS} REQUIRED)
set(GI_SHADER_DIR "${CMAKE_BINARY_DIR}/gi_shaders")
set(GI_SHADER_SHARED "${CMAKE_SOURCE_DIR}/src/passes/ray_tracing/shaders")
set(GI_SHADER_INCLUDES "${GI_SHADER_SHARED}/gi_shared.h" "${GI_SHADER_SHARED}/gi_common.hlsli" "${GI_SHADER_SHARED}/gi_shadow.hlsli" "${GI_SHADER_SHARED}/gi_hit_light.hlsli"
    "${GI_SHADER_SHARED}/ggx_specular.hlsli" "${CMAKE_SOURCE_DIR}/third_party/ffx_denoiser_shadows/ffx_denoiser_shadows_filter.h"
    "${CMAKE_SOURCE_DIR}/third_party/ffx_denoiser_shadows/ffx_denoiser_shadows_tileclassification.h"
    "${CMAKE_SOURCE_DIR}/third_party/ffx_denoiser_shadows/ffx_denoiser_shadows_util.h" "${nrd_SOURCE_DIR}/Shaders/NRD.hlsli")
set(GI_SHADER_HEADERS)
function(add_gi_kernels source)
    foreach(entry IN LISTS ARGN)
        set(output "${GI_SHADER_DIR}/${entry}.h")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${DXC_EXECUTABLE}" -nologo -T cs_6_5 -E ${entry} -HV 2021 -O3 -WX -I "${GI_SHADER_SHARED}" -I "${CMAKE_SOURCE_DIR}/third_party" -I "${nrd_SOURCE_DIR}/Shaders"
                -Fh "${output}" -Vn GI_${entry}
                "${CMAKE_SOURCE_DIR}/src/${source}"
            DEPENDS "${CMAKE_SOURCE_DIR}/src/${source}" ${GI_SHADER_INCLUDES}
            COMMENT "DXC ${source}: ${entry}")
        list(APPEND GI_SHADER_HEADERS "${output}")
    endforeach()
    set(GI_SHADER_HEADERS ${GI_SHADER_HEADERS} PARENT_SCOPE)
endfunction()
file(MAKE_DIRECTORY "${GI_SHADER_DIR}")
add_gi_kernels(passes/ray_tracing/shaders/gi_scene.hlsl ClearNewGeometries DebugScene TerrainGrid Skin)
add_gi_kernels(features/global_illumination/gi_light.hlsl ClearCounters GenerateDispatch PrepareGuides TraceLight LearnAlbedo PurgeTiles PopulateMultibounceCells PopulateCells
    UpdateTiles UpdateMultibounceCells ResolveLight UpdateFoliageMemory CompositeLight)
add_gi_kernels(features/ray_traced_shadows/gi_shadow.hlsl TraceSunShadows ClassifyShadowTiles DebugShadows ProbeShadows)
add_gi_kernels(features/ray_traced_shadows/gi_shadow_filter.hlsl FilterShadows)
add_gi_kernels(features/ray_traced_lights/light_shadows.hlsl TraceLocalLightShadows FilterLocalLightShadows ProbeLocalLights)
add_gi_kernels(features/path_tracing/reflections.hlsl TraceReflections TraceSurfaceReflections)
add_gi_kernels(features/path_tracing/direct_light.hlsl TraceLampLight)
add_custom_target(gi_kernels DEPENDS ${GI_SHADER_HEADERS})

# Each feature's settings and their description (src/features/).
set(PHANTOM_FX_FEATURES src/features/upscaling/settings.cpp src/features/frame_generation/settings.cpp src/features/sun/settings.cpp src/features/ray_traced_shadows/settings.cpp src/features/soft_shadows/settings.cpp src/features/sun_contact_shadows/settings.cpp
    src/features/ray_traced_lights/settings.cpp src/features/lamp_contact_shadows/settings.cpp src/features/lamp_soft_shadows/settings.cpp
    src/features/contact_shadow_shape/settings.cpp src/features/global_illumination/settings.cpp src/features/ambient_occlusion/settings.cpp
    src/features/indirect_light/settings.cpp src/features/path_tracing/settings.cpp src/features/volumetric_fog/settings.cpp src/features/sea/settings.cpp src/features/photo_mode/settings.cpp src/features/hdr/settings.cpp src/features/frame_rate/settings.cpp
    src/features/motion_blur/settings.cpp)

# The add-on uses ReShade's ImGui through the overlay function table, so none is linked.
add_library(phantom_fx SHARED src/addon.cpp src/core.cpp src/modules.cpp src/features/upscaling/upscaler.cpp src/features/upscaling/render_size_plan.cpp src/features/upscaling/frames.cpp src/menu.cpp src/menu_budget.cpp src/fxui/fxui.cpp src/phantom_fx_settings.cpp ${PHANTOM_FX_FEATURES} src/camera.cpp src/constant_capture.cpp
    src/device_check.cpp src/features/motion_blur/motion_blur.cpp src/features/upscaling/fxaa.cpp src/features/frame_rate/frame_rate.cpp src/render_scale.cpp src/ui_redirect.cpp src/frame_log.cpp src/resource_dump.cpp
    src/log.cpp src/shader_bytecode.cpp src/game_shaders.cpp src/game_dither.cpp src/features/frame_generation/presenter.cpp src/features/frame_generation/frame_generation.cpp src/game_loop.cpp src/passes/contact_shadow_trace.cpp src/passes/sun_light_pass.cpp src/features/sun_contact_shadows/sun_contact_shadows.cpp
    src/passes/local_light_pass.cpp src/features/lamp_contact_shadows/lamp_contact_shadows.cpp src/features/lamp_soft_shadows/lamp_soft_shadows.cpp
    src/passes/depth_of_field_pass.cpp src/passes/indirect_light_pass.cpp src/passes/visibility_bitmask.cpp src/features/ambient_occlusion/gtvbao.cpp
    src/features/indirect_light/ssilvb.cpp src/features/global_illumination/light_combine.cpp src/features/photo_mode/photo_mode.cpp src/features/photo_mode/focus.cpp src/features/hdr/hdr.cpp src/weather.cpp src/character.cpp src/game_lua.cpp src/travel.cpp src/features/volumetric_fog/atmosphere.cpp src/features/sea/sea.cpp src/sun_movement.cpp
    src/fidelityfx.cpp src/features/upscaling/fsr_upscaler.cpp src/features/upscaling/ray_reconstruction.cpp src/features/path_tracing/path_tracing.cpp src/features/path_tracing/glass.cpp src/features/path_tracing/path_tracing_rays.cpp src/features/path_tracing/sky_capture.cpp src/features/frame_generation/fsr_frame_generation.cpp src/performance.cpp src/performance_menu.cpp
    src/passes/ray_tracing/ray_tracing.cpp src/passes/ray_tracing/engine_scene.cpp src/passes/ray_tracing/scene_description.cpp src/passes/ray_tracing/engine_brush.cpp src/passes/ray_tracing/gi_inputs.cpp src/features/ray_traced_lights/local_lights.cpp src/features/ray_traced_lights/light_choice.cpp src/features/ray_traced_lights/light_shadow_rays.cpp src/passes/ray_tracing/gi_renderer.cpp src/passes/ray_tracing/gi_scene.cpp src/passes/ray_tracing/scene_contents.cpp src/passes/ray_tracing/gi_textures.cpp src/features/global_illumination/gi_light.cpp src/features/global_illumination/gi_denoiser.cpp src/features/global_illumination/gi_lighting.cpp src/features/ray_traced_shadows/gi_shadow.cpp
    "${CMAKE_BINARY_DIR}/version.rc")
add_dependencies(phantom_fx gi_kernels)
target_include_directories(phantom_fx PRIVATE src "${GI_SHADER_DIR}")
target_include_directories(phantom_fx SYSTEM PRIVATE "${RESHADE_DIR}/include" "${RESHADE_DIR}/imgui" "${STREAMLINE_DIR}/include" third_party
    "${FIDELITYFX_DIR}")
target_link_libraries(phantom_fx PRIVATE minhook ngx NRD d3d11 d3d12 dxgi d3dcompiler)
target_compile_features(phantom_fx PRIVATE cxx_std_20)
target_compile_definitions(phantom_fx PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX ImTextureID=ImU64 PHANTOM_FX_VERSION="${PROJECT_VERSION}"
    PHANTOM_FX_DIAGNOSTIC_LOG=$<BOOL:${PHANTOM_FX_DIAGNOSTIC_LOG}> PHANTOM_FX_DEV_TOOLS=$<BOOL:${PHANTOM_FX_DEV_TOOLS}>)
target_compile_options(phantom_fx PRIVATE /W4 /WX /permissive- /utf-8)
# Record only the PDB's file name, not the build machine's path.
target_link_options(phantom_fx PRIVATE /PDBALTPATH:%_PDB%)
set_target_properties(phantom_fx PROPERTIES
    OUTPUT_NAME PhantomFX
    SUFFIX .addon64
    CXX_EXTENSIONS OFF
)
# NGX loads the model DLLs, and the add-on Streamline and the FSR runtime, from the add-on's directory.
add_custom_command(TARGET phantom_fx POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${DLSS_FILES} ${STREAMLINE_FILES} ${FIDELITYFX_FILES} "$<TARGET_FILE_DIR:phantom_fx>"
)

# The add-on and its DLLs go beside mgsvtpp.exe; documentation and licenses into PhantomFX/.
install(TARGETS phantom_fx RUNTIME DESTINATION . COMPONENT addon)
install(FILES ${DLSS_FILES} ${STREAMLINE_FILES} ${FIDELITYFX_FILES} DESTINATION . COMPONENT addon)
install(FILES README.md CHANGELOG.md LICENSE DESTINATION PhantomFX COMPONENT addon)
install(FILES "${DLSS_SDK_DIR}/LICENSE.txt" DESTINATION PhantomFX RENAME NVIDIA-DLSS-LICENSE.txt COMPONENT addon)
install(FILES "${STREAMLINE_DIR}/license.txt" DESTINATION PhantomFX RENAME NVIDIA-Streamline-LICENSE.txt COMPONENT addon)
install(FILES "${FIDELITYFX_DIR}/license.md" DESTINATION PhantomFX RENAME AMD-FSR-SDK-LICENSE.txt COMPONENT addon)
install(FILES "${minhook_SOURCE_DIR}/LICENSE.txt" DESTINATION PhantomFX RENAME MinHook-LICENSE.txt COMPONENT addon)
install(FILES "${nrd_SOURCE_DIR}/LICENSE.txt" DESTINATION PhantomFX RENAME NVIDIA-NRD-LICENSE.txt COMPONENT addon)
install(FILES "${RESHADE_DIR}/LICENSE.md" DESTINATION PhantomFX RENAME ReShade-LICENSE.txt COMPONENT addon)
install(FILES "${RESHADE_DIR}/imgui/LICENSE.txt" DESTINATION PhantomFX RENAME ImGui-LICENSE.txt COMPONENT addon)
# The sharpening shader is ported from FidelityFX FSR 1 RCAS; the screen-space shadows are Bend Studio's;
# GTVBAO is ported from three-gtvbao, with XeGTAO's noise and denoiser; SSILVB gathers light as SSRT3 does.
install(FILES licenses/AMD-FidelityFX-LICENSE.txt licenses/Bend-Studio-SSS-LICENSE.txt licenses/three-gtvbao-LICENSE.txt
    licenses/XeGTAO-LICENSE.txt licenses/SSRT3-LICENSE.txt licenses/RenoDX-LICENSE.txt
    licenses/UnrealEngineSkyAtmosphere-LICENSE.txt licenses/Capsaicin-LICENSE.txt DESTINATION PhantomFX COMPONENT addon)

include(CTest)
if(BUILD_TESTING)
    add_executable(upscaler_tests tests/upscaler_tests.cpp src/features/upscaling/render_size_plan.cpp src/features/upscaling/frames.cpp src/phantom_fx_settings.cpp ${PHANTOM_FX_FEATURES} src/camera.cpp src/constant_capture.cpp
        src/log.cpp src/shader_bytecode.cpp src/features/global_illumination/gi_lighting.cpp)
    target_include_directories(upscaler_tests PRIVATE src)
    target_link_libraries(upscaler_tests PRIVATE d3d11 d3dcompiler)
    target_compile_features(upscaler_tests PRIVATE cxx_std_20)
    target_compile_definitions(upscaler_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(upscaler_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME upscaler COMMAND upscaler_tests)

    add_executable(scene_description_tests tests/scene_description_tests.cpp src/passes/ray_tracing/scene_description.cpp)
    target_include_directories(scene_description_tests PRIVATE src)
    target_compile_features(scene_description_tests PRIVATE cxx_std_20)
    target_compile_options(scene_description_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME scene_description COMMAND scene_description_tests)

    add_executable(scene_contents_tests tests/scene_contents_tests.cpp src/passes/ray_tracing/scene_contents.cpp src/passes/ray_tracing/scene_description.cpp)
    target_include_directories(scene_contents_tests PRIVATE src)
    target_compile_features(scene_contents_tests PRIVATE cxx_std_20)
    target_compile_definitions(scene_contents_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(scene_contents_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME scene_contents COMMAND scene_contents_tests)

    add_executable(light_choice_tests tests/light_choice_tests.cpp src/features/ray_traced_lights/light_choice.cpp)
    target_include_directories(light_choice_tests PRIVATE src)
    target_compile_features(light_choice_tests PRIVATE cxx_std_20)
    target_compile_definitions(light_choice_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(light_choice_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME light_choice COMMAND light_choice_tests)

    add_executable(game_shaders_tests tests/game_shaders_tests.cpp src/game_shaders.cpp src/log.cpp src/shader_bytecode.cpp)
    target_include_directories(game_shaders_tests PRIVATE src)
    target_link_libraries(game_shaders_tests PRIVATE d3d11 d3dcompiler)
    target_compile_features(game_shaders_tests PRIVATE cxx_std_20)
    target_compile_definitions(game_shaders_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(game_shaders_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME game_shaders COMMAND game_shaders_tests)

    add_executable(shader_bytecode_tests tests/shader_bytecode_tests.cpp src/shader_bytecode.cpp)
    target_include_directories(shader_bytecode_tests PRIVATE src)
    target_link_libraries(shader_bytecode_tests PRIVATE d3d11 d3dcompiler)
    target_compile_features(shader_bytecode_tests PRIVATE cxx_std_20)
    target_compile_definitions(shader_bytecode_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(shader_bytecode_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME shader_bytecode COMMAND shader_bytecode_tests)

    add_executable(command_list_notes_tests tests/command_list_notes_tests.cpp)
    target_include_directories(command_list_notes_tests PRIVATE src)
    target_compile_features(command_list_notes_tests PRIVATE cxx_std_20)
    target_compile_options(command_list_notes_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME command_list_notes COMMAND command_list_notes_tests)
endif()

# Package only runtime files and documentation, not dependency development files.
set(CPACK_GENERATOR ZIP)
set(CPACK_COMPONENTS_ALL addon)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_ARCHIVE_ADDON_FILE_NAME "PhantomFX-${PROJECT_VERSION}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_CHECKSUM SHA256)
include(CPack)
