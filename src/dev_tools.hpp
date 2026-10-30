#pragma once

#ifndef PHANTOM_FX_DEV_TOOLS
#define PHANTOM_FX_DEV_TOOLS 0
#endif

namespace phantom_fx
{
// The dev preset's build: the overlay's Developer page, with travel between the maps (travel.hpp)
// and the Lua console (game_lua.hpp), and the debug cases (automation.hpp). Their code is left out of a
// release build altogether: CMakeLists.txt drops their files, and the rest stands in #if PHANTOM_FX_DEV_TOOLS.
inline constexpr bool DevTools = PHANTOM_FX_DEV_TOOLS != 0;
}
