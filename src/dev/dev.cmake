# The development tools (README.md here), in a dev build alone. Their files hook the system's file calls, patch Steam's
# interfaces and run what they are told, as security software rightly dislikes, so none of this is in a release build.
set(PHANTOM_FX_DEV_SOURCES src/dev/dev.cpp src/dev/automation.cpp src/dev/save_redirect.cpp src/dev/game_lua.cpp src/dev/travel.cpp src/dev/cutscenes.cpp)
