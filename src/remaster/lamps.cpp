#include "remaster/lamps.hpp"

#include <algorithm>

namespace phantom_fx::remaster
{
namespace
{
// Float positions read back from the file and from a draw's constants differ by rounding alone.
constexpr float MatchMetres = 0.05f;
}

bool isLampAt(const std::array<float, 3>& place, const std::array<float, 3>& at)
{
    const float x = place[0] - at[0], y = place[1] - at[1], z = place[2] - at[2];

    return x * x + y * y + z * z <= MatchMetres * MatchMetres;
}

const LampRule* lampRuleAt(std::span<const LampRule> rules, std::uint16_t location, const std::array<float, 3>& at)
{
    const auto found = std::ranges::find_if(rules, [&](const LampRule& rule) { return rule.location == location && isLampAt(rule.at, at); });

    return found != rules.end() ? &*found : nullptr;
}
}
