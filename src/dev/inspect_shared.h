// A debug case's record of a pixel, shared by inspect.hlsl and inspect_trace.cpp. A hit is its instance + 1 with its
// geometry above GI_HIT_INSTANCE_BITS, 0 for none; a distance is along its ray.
#ifndef INSPECT_SHARED_H
#define INSPECT_SHARED_H

#ifdef __cplusplus
#include "passes/ray_tracing/shaders/gi_shared.h"

namespace phantom_fx::gi
{
#endif

struct GIInspectPixel
{
    uint viewHit;          // What the camera's ray meets first
    float viewDistance;
    uint surfaceHit;       // What the scene has at the game's surface (GI_INSPECT_SURFACE_* in inspect.hlsl)
    float surfaceDistance;
    uint sunHit;           // What blocks the ray to the sun's centre, as the shadow rays trace it
    float sunDistance;
    float gameDistance;    // The game's surface from the eye, 0 for its sky
    uint kinds;            // The three hits' GI_INSTANCE_*, a byte each, the camera's lowest
};

#ifdef __cplusplus
static_assert(sizeof(GIInspectPixel) == 32);
}
#endif

#endif
