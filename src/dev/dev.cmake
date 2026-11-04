# The development tools (README.md here), in a dev build alone. Their files hook the system's file calls, patch Steam's
# interfaces and run what they are told, as security software rightly dislikes, so none of this is in a release build.
set(PHANTOM_FX_DEV_SOURCES src/dev/dev.cpp src/dev/automation.cpp src/dev/save_redirect.cpp src/dev/game_lua.cpp src/dev/travel.cpp src/dev/cutscenes.cpp
    src/dev/inspect_trace.cpp src/dev/game_shadow.cpp src/dev/held_camera.cpp src/dev/perf_record.cpp src/dev/gpu_trace.cpp src/dev/remaster/tool.cpp src/dev/remaster/panel.cpp src/dev/remaster/marks.cpp)

# Nsight Graphics' SDK, from its install if there is one: the automation's `gputrace` (gpu_trace.hpp) needs it.
file(TO_CMAKE_PATH "$ENV{ProgramW6432}" PROGRAM_FILES) # The 64-bit folder, whatever started the build
file(GLOB NGFX_SDK_HEADERS "${PROGRAM_FILES}/NVIDIA Corporation/Nsight Graphics */SDKs/NsightGraphicsSDK/*/include/NGFX_GPUTrace_D3D12.h")
if(NGFX_SDK_HEADERS)
    list(SORT NGFX_SDK_HEADERS COMPARE NATURAL ORDER DESCENDING)
    list(GET NGFX_SDK_HEADERS 0 NGFX_SDK_HEADER)
    get_filename_component(NGFX_SDK_INCLUDE "${NGFX_SDK_HEADER}" DIRECTORY)
    set_source_files_properties(src/dev/gpu_trace.cpp PROPERTIES COMPILE_OPTIONS "/external:I${NGFX_SDK_INCLUDE};/external:W0" COMPILE_DEFINITIONS PHANTOM_FX_NSIGHT_SDK=1)
else()
    set_source_files_properties(src/dev/gpu_trace.cpp PROPERTIES COMPILE_DEFINITIONS PHANTOM_FX_NSIGHT_SDK=0)
endif()

# The remastering tool saves into this checkout.
set_source_files_properties(src/dev/remaster/tool.cpp PROPERTIES COMPILE_DEFINITIONS "PHANTOM_FX_MATERIALS_FILE=\"${CMAKE_SOURCE_DIR}/src/remaster/materials.json\"")

# The record's kernel takes the shadow rays' file whole, so a change there makes it again.
list(APPEND GI_SHADER_INCLUDES "${CMAKE_SOURCE_DIR}/src/features/ray_traced_shadows/gi_shadow.hlsl" "${CMAKE_SOURCE_DIR}/src/dev/inspect_shared.h")
add_gi_kernels(dev/inspect.hlsl InspectPixels)
