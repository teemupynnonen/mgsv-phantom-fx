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
    // The waves' shape against the weather's sea, in the ini alone: the peak frequency (below 1, longer waves),
    // the height (1 is JONSWAP's), the ripples too short for the tiles, the crests' sharpness, and where one
    // tile's waves end and the next's begin (larger, the longer waves take more of the spectrum).
    float peak{0.7f};
    float height{0.85f};
    float ripples{0.5f};
    float chop{0.9f};
    float split{4};
    // How far short of folding foam shows on the crests: 1 is only where the waves squeeze hardest.
    float foamCoverage{1.25f};
    // The water's albedo, linear; PhantomFX.ini only, for matching the game's sea (GI_SEA_ALBEDO's).
    float albedo[3]{0.012f, 0.066f, 0.123f};

    bool operator==(const Sea&) const = default;
};

void describe(Sea& sea, SettingVisitor& visit);
}
