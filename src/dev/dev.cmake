# The development tools (README.md here), in a dev build alone. Their files hook the system's file calls, patch Steam's
# interfaces and run what they are told, as security software rightly dislikes, so none of this is in a release build.
set(PHANTOM_FX_DEV_SOURCES src/dev/dev.cpp src/dev/automation.cpp src/dev/save_redirect.cpp src/dev/game_lua.cpp src/dev/travel.cpp src/dev/cutscenes.cpp
    src/dev/inspect_trace.cpp src/dev/game_shadow.cpp src/dev/held_camera.cpp)

# The record's kernel takes the shadow rays' file whole, so a change there makes it again.
list(APPEND GI_SHADER_INCLUDES "${CMAKE_SOURCE_DIR}/src/features/ray_traced_shadows/gi_shadow.hlsl" "${CMAKE_SOURCE_DIR}/src/dev/inspect_shared.h")
add_gi_kernels(dev/inspect.hlsl InspectPixels)
