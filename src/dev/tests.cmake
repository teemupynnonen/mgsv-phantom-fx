# The development tools' tests, in a dev build alone, after remaster.cmake has the JSON header.
add_executable(marks_tests src/dev/remaster/marks_tests.cpp src/dev/remaster/marks.cpp src/remaster/rules.cpp)
target_include_directories(marks_tests PRIVATE src)
target_include_directories(marks_tests SYSTEM PRIVATE "${NLOHMANN_JSON_DIR}/include")
target_compile_features(marks_tests PRIVATE cxx_std_20)
target_compile_definitions(marks_tests PRIVATE PHANTOM_FX_DEV_TOOLS=1)
target_compile_options(marks_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME marks COMMAND marks_tests)
