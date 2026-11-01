#pragma once

#ifndef PHANTOM_FX_DEV_TOOLS
#define PHANTOM_FX_DEV_TOOLS 0
#endif

namespace phantom_fx
{
// The dev preset's build: the overlay's Developer page and its tools. Players never see it.
inline constexpr bool DevTools = PHANTOM_FX_DEV_TOOLS != 0;
}
