#pragma once

#ifndef PHANTOM_FX_DEV_TOOLS
#define PHANTOM_FX_DEV_TOOLS 0
#endif

namespace phantom_fx
{
// The dev preset's build: the overlay's Developer page and its tools, and the debug cases (dev/README.md). Players
// never see them: their code is left out of a release build altogether. CMakeLists.txt drops their files, and the rest
// stands in #if PHANTOM_FX_DEV_TOOLS.
inline constexpr bool DevTools = PHANTOM_FX_DEV_TOOLS != 0;
}
