#pragma once

#ifndef PHANTOM_FX_DEV_TOOLS
#define PHANTOM_FX_DEV_TOOLS 0
#endif

namespace phantom_fx
{
// The dev preset's build: the overlay's Developer page, with travel between the maps (travel.hpp),
// the cutscene player (cutscenes.hpp) and the Lua console (game_lua.hpp). Players never see it.
inline constexpr bool DevTools = PHANTOM_FX_DEV_TOOLS != 0;
}
