# The devkit's tools as their own add-on (README.md here): DevKit.addon64, built in the dev preset
# beside PhantomFX and never linked into it.
add_library(devkit_addon SHARED src/devaddon/addon.cpp src/devaddon/game_loop.cpp src/resource_dump.cpp)
target_include_directories(devkit_addon PRIVATE src)
target_include_directories(devkit_addon SYSTEM PRIVATE "${RESHADE_DIR}/include" "${RESHADE_DIR}/imgui")
target_link_libraries(devkit_addon PRIVATE devkit phantom_fx_core d3d11 d3dcompiler)
target_compile_features(devkit_addon PRIVATE cxx_std_20)
target_compile_definitions(devkit_addon PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX ImTextureID=ImU64)
target_compile_options(devkit_addon PRIVATE /W4 /WX /permissive- /utf-8)
set_target_properties(devkit_addon PROPERTIES
    OUTPUT_NAME DevKit
    # Not devkit.lib: Windows ignores case, and the static library is already there.
    ARCHIVE_OUTPUT_NAME DevKit_import
    SUFFIX .addon64
    CXX_EXTENSIONS OFF
)
