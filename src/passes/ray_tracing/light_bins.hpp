#pragma once

#include "passes/ray_tracing/shaders/gi_shared.h"

// Which of the frame's lights can reach a place, so a pixel or a ray's hit walks those and not all of them (docs/DEVELOPMENT.md,
// "Light bins"). The bins run along each world axis around the eye, and each holds a bit for every light whose bounding box crosses
// it. A place's three bins share the bits of the boxes around it, as a grid of every cell would, in a fraction of the room.
//
// A light's box only ever holds too much: a light left out of a place gives it no light, so the light that reaches a place, and the
// random numbers it draws, stay as they were with every light walked.
namespace phantom_fx::light_bins
{
// Sets `constants`' bins from its eye and its lights.
void fill(gi::GIFrameConstants& constants) noexcept;
}
