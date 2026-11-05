#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// The sea (sea.hpp): waves from an ocean spectrum, drawn into the G-buffer in place of the game's sea.
struct Sea
{
    bool on{};
    // How fast the waves travel against real time; the game's own sea rolls at about half.
    float speed{0.5f};
    // The waves' shape against the weather's sea: the peak frequency (below 1, longer waves), the geometry's
    // height, the ripples too short for the tiles, the crests' sharpness, and where one tile's waves end and the
    // next's begin (larger, the longer waves take more of the spectrum).
    float peak{0.7f};
    float height{1.1f};
    float ripples{2};
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
