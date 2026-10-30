#include "modules.hpp"

#include "features/ambient_occlusion/gtvbao.hpp"
#include "features/frame_rate/frame_rate.hpp"
#include "features/global_illumination/light_combine.hpp"
#include "features/indirect_light/ssilvb.hpp"
#include "features/lamp_contact_shadows/lamp_contact_shadows.hpp"
#include "features/lamp_soft_shadows/lamp_soft_shadows.hpp"
#include "features/motion_blur/motion_blur.hpp"
#include "features/path_tracing/path_tracing.hpp"
#include "features/path_tracing/sky_capture.hpp"
#include "features/photo_mode/focus.hpp"
#include "features/photo_mode/photo_mode.hpp"
#include "features/ray_traced_lights/local_lights.hpp"
#include "features/sea/sea.hpp"
#include "features/sun_contact_shadows/sun_contact_shadows.hpp"
#include "features/volumetric_fog/atmosphere.hpp"
#include "passes/contact_shadow_trace.hpp"
#include "passes/indirect_light_pass.hpp"
#include "passes/local_light_pass.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "passes/sun_light_pass.hpp"
#include "passes/visibility_bitmask.hpp"

namespace phantom_fx::modules
{
namespace
{
struct Module
{
    void (*attach)(ID3D11Device* device, ID3D11DeviceContext* immediate) noexcept{};
    void (*configure)(const Configuration& configuration) noexcept{};
    void (*present)() noexcept{};
    void (*detach)() noexcept{};
};

// For a module that needs only the device.
template<void (*Attach)(ID3D11Device*) noexcept> void withDevice(ID3D11Device* device, ID3D11DeviceContext*) noexcept
{
    Attach(device);
}

// In the order they attach, configure, present and detach; a module leaves out what it does not do.
constexpr Module Modules[]{
    {.attach = withDevice<sun_light_pass::attach>, .configure = sun_light_pass::configure, .present = sun_light_pass::present, .detach = sun_light_pass::detach},
    {.present = sun_contact_shadows::present, .detach = sun_contact_shadows::detach},
    {.configure = local_light_pass::configure, .present = local_light_pass::present},
    {.attach = withDevice<lamp_contact_shadows::attach>, .configure = lamp_contact_shadows::configure, .present = lamp_contact_shadows::present,
        .detach = lamp_contact_shadows::detach},
    {.attach = withDevice<lamp_soft_shadows::attach>, .configure = lamp_soft_shadows::configure, .detach = lamp_soft_shadows::detach},
    {.attach = withDevice<contact_shadow_trace::attach>, .configure = contact_shadow_trace::configure, .present = contact_shadow_trace::present,
        .detach = contact_shadow_trace::detach},

    {.attach = withDevice<indirect_light_pass::attach>, .configure = indirect_light_pass::configure, .present = indirect_light_pass::present,
        .detach = indirect_light_pass::detach},
    {.attach = withDevice<gtvbao::attach>, .configure = gtvbao::configure, .present = gtvbao::present, .detach = gtvbao::detach},
    {.attach = withDevice<ssilvb::attach>, .configure = ssilvb::configure, .present = ssilvb::present, .detach = ssilvb::detach},
    {.attach = withDevice<light_combine::attach>, .configure = light_combine::configure, .present = light_combine::present, .detach = light_combine::detach},
    {.attach = withDevice<path_tracing::attach>, .configure = path_tracing::configure, .present = path_tracing::present, .detach = path_tracing::detach},
    {.attach = withDevice<sky_capture::attach>, .configure = sky_capture::configure, .present = sky_capture::present, .detach = sky_capture::detach},
    {.attach = withDevice<visibility_bitmask::attach>, .configure = visibility_bitmask::configure, .detach = visibility_bitmask::detach},

    {.attach = ray_tracing::attach, .configure = ray_tracing::configure, .present = ray_tracing::present, .detach = ray_tracing::detach},
    {.attach = local_lights::attach, .configure = local_lights::configure, .present = local_lights::present, .detach = local_lights::detach},
    {.attach = atmosphere::attach, .configure = atmosphere::configure, .present = atmosphere::present, .detach = atmosphere::detach},
    {.attach = sea::attach, .configure = sea::configure, .present = sea::present, .detach = sea::detach},

    {.configure = motion_blur::configure},
    {.configure = frame_rate::configure},
    {.configure = photo_mode::configure},
    {.detach = focus::detach},
};
}

void attach(ID3D11Device* device, ID3D11DeviceContext* immediate) noexcept
{
    for (const auto& module : Modules)
        if (module.attach) module.attach(device, immediate);
}

void detach() noexcept
{
    for (const auto& module : Modules)
        if (module.detach) module.detach();
}

void configure(const Configuration& configuration) noexcept
{
    for (const auto& module : Modules)
        if (module.configure) module.configure(configuration);
}

void present() noexcept
{
    for (const auto& module : Modules)
        if (module.present) module.present();
}
}
