#pragma once

#include "features/setting_visitor.hpp"

#include <cstdint>

namespace phantom_fx
{
// What blurs what is behind the focus. The focus edges are the render size's with either.
enum class FarBlur : std::uint8_t { Lens, Mgsv };

struct DepthOfField
{
    FarBlur farBlur{FarBlur::Lens};

    bool operator==(const DepthOfField&) const = default;
};

void describe(DepthOfField& depthOfField, SettingVisitor& visit);
}
