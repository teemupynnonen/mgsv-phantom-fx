#include "passes/ray_tracing/trace.hpp"

#include "dev/inspect_trace.hpp"
#include "dev_tools.hpp"
#include "features/ray_traced_lights/lamp_shadow_rays.hpp"

namespace phantom_fx::trace
{
std::span<Trace* const> inOrder() noexcept
{
#if PHANTOM_FX_DEV_TOOLS
    static Trace* const traces[]{&lamp_shadow_rays::trace(), &inspect_trace::trace()};
#else
    static Trace* const traces[]{&lamp_shadow_rays::trace()};
#endif

    return traces;
}
}
