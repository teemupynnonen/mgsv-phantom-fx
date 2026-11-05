#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct PhotoMode
{
    float fNumber{1.4f};
    bool noNearClip{true};

    bool operator==(const PhotoMode&) const = default;
};

void describe(PhotoMode& photoMode, SettingVisitor& visit);
}
