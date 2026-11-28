#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// HDR10 output (hdr.hpp). Read at start: with `hot` the float scene runs from start whatever `on`
// says, so HDR output turns on and off while the game runs; without it, the scene is the game's own
// 8-bit one unless `on` is set at start, and HDR takes a restart. The brightnesses apply at once.
struct Hdr
{
    bool on{};
    bool hot{};
    float peakNits{};
    float paperWhiteNits{203};

    bool operator==(const Hdr&) const = default;
};

void describe(Hdr& hdr, SettingVisitor& visit);
}
