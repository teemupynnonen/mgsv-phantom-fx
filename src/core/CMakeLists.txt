# The parts every add-on shares: no ReShade calls, no overlay, no game hooks of its own, so it builds
# and tests on its own. A second add-on links it with
#
#     add_subdirectory(<this directory> core)
#     target_link_libraries(my_addon PRIVATE phantom_fx_core)
#
# See docs/addon-core.md.
cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(phantom_fx_core LANGUAGES CXX)
    enable_testing()
    set(PHANTOM_FX_CORE_TESTS_DEFAULT ON)
else()
    # A consumer does not build another project's tests unless it asks.
    set(PHANTOM_FX_CORE_TESTS_DEFAULT OFF)
endif()
option(PHANTOM_FX_CORE_TESTS "Build phantom_fx_core's tests" ${PHANTOM_FX_CORE_TESTS_DEFAULT})

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "phantom_fx_core requires Windows and MSVC.")
endif()

# game_code.hpp includes MinHook. A consumer that already has it (PhantomFX) skips the download.
if(NOT TARGET minhook)
    include(FetchContent)
    FetchContent_Declare(minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537 # v1.3.4
    )
    FetchContent_MakeAvailable(minhook)
endif()

# The add-on source root: `core/...` resolves from it, and the headers that moved out of the
# add-on keep their names, so `log.hpp` and `game_shaders.hpp` resolve from here too.
set(PHANTOM_FX_CORE_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(phantom_fx_core STATIC
    order.cpp
    log.cpp
    settings/ini.cpp
    shader_bytecode.cpp
    game/camera.cpp
    game/game_shaders.cpp
    game/device_check.cpp
    capture/constant_capture.cpp
)
target_include_directories(phantom_fx_core PUBLIC
    "${PHANTOM_FX_CORE_INCLUDE}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/game"
    "${CMAKE_CURRENT_SOURCE_DIR}/capture"
)
target_compile_features(phantom_fx_core PUBLIC cxx_std_20)
target_compile_definitions(phantom_fx_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(phantom_fx_core PRIVATE /W4 /WX /permissive- /utf-8)
target_link_libraries(phantom_fx_core PUBLIC d3d11 d3dcompiler minhook)
set_target_properties(phantom_fx_core PROPERTIES CXX_EXTENSIONS OFF)

include(CTest)
if(PHANTOM_FX_CORE_TESTS AND BUILD_TESTING)
    add_executable(core_order_tests tests/core_order_tests.cpp)
    target_link_libraries(core_order_tests PRIVATE phantom_fx_core)
    target_compile_options(core_order_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME core_order COMMAND core_order_tests)

    add_executable(core_settings_tests tests/core_settings_tests.cpp)
    target_link_libraries(core_settings_tests PRIVATE phantom_fx_core)
    target_compile_options(core_settings_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME core_settings COMMAND core_settings_tests)
endif()
