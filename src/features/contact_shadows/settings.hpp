#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// The shape of the contact shadows' trace (contact_shadow_trace.hpp), which the sun's and the lamps'
// share.
struct ContactShadowShape
{
    float contrast{1};
    float reachPixels{80};
    float thickness{0.005f}; // A share of the depth between a sample and the far plane
    float fadeMetres{500};

    bool operator==(const ContactShadowShape&) const = default;
};

// Short screen-space shadows of the sun and the lamps, one switch for both.
struct ContactShadows
{
    bool on{};
    ContactShadowShape shape;

    bool operator==(const ContactShadows&) const = default;
};

void describe(ContactShadows& shadows, SettingVisitor& visit);
}
