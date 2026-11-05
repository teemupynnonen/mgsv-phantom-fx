#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// The sea (sea.hpp): waves from an ocean spectrum, drawn into the G-buffer in place of the game's sea.
struct Sea
{
    bool on{};
    float waves{1};
    // How fast the waves' time runs against real time; PhantomFX.ini only, for matching the game's sea.
    float speed{0.5f};
    // The water's albedo, linear; PhantomFX.ini only, for matching the game's sea (GI_SEA_ALBEDO's).
    float albedo[3]{0.012f, 0.066f, 0.123f};

    bool operator==(const Sea&) const = default;
};

void describe(Sea& sea, SettingVisitor& visit);
}
