#pragma once

#include <string>

namespace phantom_fx
{
// During start-up the game quits when the methods of its Direct3D 11 device context do not all
// point into d3d11.dll, which is the case under ReShade and RenderDoc. ReShade loads the add-on
// while the game creates its device, before that check runs, so the add-on can make it pass.
// Returns what was done, for the log.
std::string passDeviceCheck() noexcept;
// Whether mgsvtpp.exe is the build the add-on knows (PE TimeDateStamp and SizeOfImage); its engine
// addresses and render pipeline are only known for that one.
bool isSupportedGameBuild() noexcept;
}
