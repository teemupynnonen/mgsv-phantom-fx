#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// Path tracing (path_tracing.hpp): with DLSS Ray Reconstruction denoising, the reflections, the sun and the
// point and spot lights traced too.
struct PathTracing
{
    bool on{};
    float reflections{1};
    float emission{1};

    bool operator==(const PathTracing&) const = default;
};

void describe(PathTracing& pathTracing, SettingVisitor& visit);
}
