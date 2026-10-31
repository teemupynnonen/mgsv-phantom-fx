# The remastering tool (src/remaster/README.md). All of it hangs from this file, so that deleting the
# folder and the few lines its README lists leaves the add-on as it was.

# nlohmann/json 3.12.0, the one header, for the rule file.
set(NLOHMANN_JSON_COMMIT 55f93686c01528224f448c19128836e7df245f72) # v3.12.0
set(NLOHMANN_JSON_DIR "${CMAKE_BINARY_DIR}/_deps/nlohmann-json")
set(json "nlohmann/json/${NLOHMANN_JSON_COMMIT}")
download_pinned("${NLOHMANN_JSON_DIR}"
    "${json}/LICENSE.MIT|LICENSE.MIT|46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603"
    "${json}/single_include/nlohmann/json.hpp|include/nlohmann/json.hpp|aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63"
)

# The rules go inside the add-on, in every build.
set(MATERIALS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/remaster/materials.json")
set(MATERIALS_RESOURCE "${CMAKE_BINARY_DIR}/remaster_materials.rc")
configure_file(src/remaster/materials.rc.in "${MATERIALS_RESOURCE}" @ONLY)
set_source_files_properties("${MATERIALS_RESOURCE}" PROPERTIES OBJECT_DEPENDS "${MATERIALS_FILE}")

target_sources(phantom_fx PRIVATE src/remaster/remaster.cpp src/remaster/rules.cpp src/remaster/rule_file.cpp src/remaster/game_materials.cpp "${MATERIALS_RESOURCE}")
target_include_directories(phantom_fx SYSTEM PRIVATE "${NLOHMANN_JSON_DIR}/include")
install(FILES "${NLOHMANN_JSON_DIR}/LICENSE.MIT" DESTINATION PhantomFX RENAME nlohmann-json-LICENSE.txt COMPONENT addon)

# The tool itself only in a dev build, which saves into this checkout.
if(PHANTOM_FX_DEV_TOOLS)
    target_sources(phantom_fx PRIVATE src/remaster/tool.cpp)
    set_source_files_properties(src/remaster/tool.cpp PROPERTIES COMPILE_DEFINITIONS "PHANTOM_FX_MATERIALS_FILE=\"${MATERIALS_FILE}\"")
endif()

if(BUILD_TESTING)
    add_executable(remaster_tests tests/remaster_tests.cpp src/remaster/rules.cpp src/remaster/rule_file.cpp)
    target_include_directories(remaster_tests PRIVATE src)
    target_include_directories(remaster_tests SYSTEM PRIVATE "${NLOHMANN_JSON_DIR}/include")
    target_compile_features(remaster_tests PRIVATE cxx_std_20)
    target_compile_options(remaster_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME remaster COMMAND remaster_tests)
endif()
