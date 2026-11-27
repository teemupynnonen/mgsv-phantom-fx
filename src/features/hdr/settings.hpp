#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// HDR10 output (hdr.hpp). It and the brightnesses apply from the next frame: the float scene's swap
// chain is made for HDR whether it is on or not.
struct Hdr
{
    bool on{};
    float peakNits{};
    float paperWhiteNits{203};

    bool operator==(const Hdr&) const = default;
};

void describe(Hdr& hdr, SettingVisitor& visit);
}
