#include "passes/ray_tracing/trace.hpp"

#include "features/ray_traced_lights/lamp_shadow_rays.hpp"

namespace phantom_fx::trace
{
std::span<Trace* const> inOrder() noexcept
{
    static Trace* const traces[]{&lamp_shadow_rays::trace()};

    return traces;
}
}
