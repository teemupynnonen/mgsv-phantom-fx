# The development tools another MGSV add-on can build on, in a dev build alone (README.md here).
add_library(devkit STATIC src/devkit/log.cpp src/devkit/host.cpp src/devkit/save_redirect.cpp src/devkit/commands.cpp src/devkit/automation.cpp src/devkit/keys.cpp src/devkit/gamepad.cpp src/devkit/frame_log.cpp src/devkit/game_lua.cpp src/devkit/travel.cpp src/devkit/cutscenes.cpp src/devkit/page.cpp)
target_include_directories(devkit PUBLIC src)
# The page draws through ReShade's overlay function table, like the add-on's own windows.
target_include_directories(devkit SYSTEM PRIVATE "${RESHADE_DIR}/imgui" "${RESHADE_DIR}/include")
# The core's log, shader bytecode and device checks (its headers are found through it).
target_link_libraries(devkit PUBLIC minhook phantom_fx_core)
target_compile_features(devkit PUBLIC cxx_std_20)
target_compile_definitions(devkit PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(devkit PRIVATE ImTextureID=ImU64)
target_compile_options(devkit PRIVATE /W4 /WX /permissive- /utf-8)
