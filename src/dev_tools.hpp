#pragma once

#ifndef PHANTOM_FX_DEV_TOOLS
#define PHANTOM_FX_DEV_TOOLS 0
#endif

#ifndef PHANTOM_FX_TOOLS_ONLY
#define PHANTOM_FX_TOOLS_ONLY 0
#endif

namespace phantom_fx
{
// The dev preset's build: the overlay's Developer page and its tools, and the debug cases (dev/README.md). Players
// never see them: their code is left out of a release build altogether. CMake drops their files, and what the core keeps
// for them stands behind DevTools or #if PHANTOM_FX_DEV_TOOLS.
inline constexpr bool DevTools = PHANTOM_FX_DEV_TOOLS != 0;
// The tools build (CMake's PHANTOM_FX_TOOLS_ONLY): the game's frame stays as the game draws it. Every change PhantomFX
// makes is off whatever the settings say, and the game's shaders go unpatched, so another mod's rendering is what is
// debugged. Its tools that read PhantomFX's own passes have nothing to read and say so.
inline constexpr bool ToolsOnly = PHANTOM_FX_TOOLS_ONLY != 0;
}
