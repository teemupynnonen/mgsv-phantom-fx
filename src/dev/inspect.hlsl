// A debug case's record of every pixel (automation.hpp), compared with the game's own answers there. It asks what the
// sun's shadow rays and their debug view ask, so it takes their file whole; -E leaves their kernels out.
#include "../features/ray_traced_shadows/gi_shadow.hlsl"
#include "inspect_shared.h"

// How far off the game's surface a scene surface still counts as that surface, along the camera's ray: as the tests of
// the depth are, so far parts of the terrain's coarse rings match. As SURFACE_METRES and SURFACE_SHARE in case.py.
#define GI_INSPECT_SURFACE_METRES 0.05f
#define GI_INSPECT_SURFACE_SHARE  0.02f

RWStructuredBuffer<GIInspectPixel> g_Inspect : register(u97);

uint InspectedHit(SceneHit hit)
{
    return hit.hit ? (hit.instance + 1) | (hit.geometry << GI_HIT_INSTANCE_BITS) : 0u;
}

uint InspectedKind(SceneHit hit, uint shift)
{
    return hit.hit ? InstanceKind(g_Instances[hit.instance]) << shift : 0u;
}

[numthreads(8, 8, 1)]
void InspectPixels(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    float3 eye = g_Frame.eye.xyz;
    float3 direction = normalize(DepthPosition(did, 1e-6f) - eye);
    SceneHit view = TraceClosest(eye, direction, 1e5f);

    GIInspectPixel pixel = (GIInspectPixel)0;
    pixel.viewHit = InspectedHit(view);
    pixel.viewDistance = view.distance;
    pixel.kinds = InspectedKind(view, 0);

    float depth = g_Depth.Load(int3(did, 0));
    if (depth > 0.0f)
    {
        float3 position = DepthPosition(did, depth);
        pixel.gameDistance = distance(position, eye);

        // Past whatever else lies on the way: in a revealed scene, hidden copies in front of what the game drew.
        float tolerance = GI_INSPECT_SURFACE_METRES + GI_INSPECT_SURFACE_SHARE * pixel.gameDistance;
        SceneHit surface = TraceClosest(eye, direction, max(pixel.gameDistance - tolerance, 0.0f), pixel.gameDistance + tolerance);
        pixel.surfaceHit = InspectedHit(surface);
        pixel.surfaceDistance = surface.distance;
        pixel.kinds |= InspectedKind(surface, 8);

        // Without the shadows the frame has no ray length, and no sun to speak of.
        if (g_Frame.shadowParams.z > 0.0f)
        {
            DebugRay r = TraceDebugRay(position, DecodeNormal(g_Normal.Load(int3(did, 0))), depth);
            SceneHit blocker = r.anyHit;
            if (r.farHit.hit) blocker = r.farHit;
            if (r.nearHit.hit) blocker = r.nearHit;
            pixel.sunHit = InspectedHit(blocker);
            pixel.sunDistance = blocker.distance;
            pixel.kinds |= InspectedKind(blocker, 16);
        }
    }

    g_Inspect[did.y * g_Frame.dimensions.x + did.x] = pixel;
}
