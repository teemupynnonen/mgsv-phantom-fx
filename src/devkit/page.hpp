#pragma once

// The devkit's own Developer page: plain ImGui, in the host add-on's overlay, for the tools the
// devkit owns (travel, cutscenes, the Lua console). No fxui and no window of its own: the host calls
// draw() inside the window its overlay callback gets, or inside its own settings window.
namespace devkit::page
{
void draw();
}
