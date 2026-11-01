#include "passes/ray_tracing/light_bins.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace phantom_fx::light_bins
{
namespace
{
using gi::uint4;

static_assert(GI_MAX_LOCAL_LIGHTS == 4 * 32, "A bin holds four words of a bit a light.");

// Past a light's box, metres: what LocalLightReaching gives past its volume, and room for each side's rounding, as a place the
// shaders put on a bin's edge may not land in the same bin here.
constexpr float BoxMargin = GI_LIGHT_VOLUME_MARGIN + 0.03f;

void setBit(uint4& bin, std::uint32_t light) noexcept
{
    const std::array words{&bin.x, &bin.y, &bin.z, &bin.w};
    *words[light / 32] |= 1u << (light % 32);
}

// The bin along an axis of a coordinate `offset` metres past the bins' origin: past the ends, the last at that end, as
// LightsNear in gi_common.hlsli has it. Kept to the bins before it turns into an int: a reach past an int's bins, about 4,300,000 km,
// comes out of the cast as the lowest int, the first bin, and its light would go dark.
int binOf(float offset, int bins) noexcept
{
    const float bin = std::floor(offset / GI_LIGHT_BIN_SIZE);
    if (!(bin > 0)) return 0;

    return bin < bins ? static_cast<int>(bin) : bins - 1;
}

// The light's bit in the bins from `low` to `high`, metres past the bins' origin.
void mark(uint4* bins, int count, std::uint32_t light, float low, float high) noexcept
{
    const int last = binOf(high, count);
    for (int bin = binOf(low, count); bin <= last; ++bin) setBit(bins[bin], light);
}
}

void fill(gi::GIFrameConstants& constants) noexcept
{
    const auto across = static_cast<int>(GI_LIGHT_BINS_ACROSS), high = static_cast<int>(GI_LIGHT_BINS_HIGH);
    const std::array<float, 3> origin{constants.eye.x - GI_LIGHT_BIN_SIZE * across / 2, constants.eye.y - GI_LIGHT_BIN_SIZE * high / 2,
        constants.eye.z - GI_LIGHT_BIN_SIZE * across / 2};
    constants.lightBinsOrigin = {origin[0], origin[1], origin[2], 0};
    std::memset(constants.lightBinsX, 0, sizeof(constants.lightBinsX));
    std::memset(constants.lightBinsY, 0, sizeof(constants.lightBinsY));
    std::memset(constants.lightBinsZ, 0, sizeof(constants.lightBinsZ));

    uint4* const bins[]{constants.lightBinsX, constants.lightBinsY, constants.lightBinsZ};
    const int counts[]{across, high, across};
    for (std::uint32_t i = 0; i < constants.localLightCount.x; ++i)
    {
        const auto& light = constants.localLights[i];
        const std::array position{light.position.x, light.position.y, light.position.z};
        const std::array volumeLow{light.volumeLow.x, light.volumeLow.y, light.volumeLow.z};
        const std::array volumeHigh{light.volumeHigh.x, light.volumeHigh.y, light.volumeHigh.z};
        for (int axis = 0; axis < 3; ++axis)
        {
            float low = position[axis] - light.position.w, top = position[axis] + light.position.w;
            if (light.volumeLow.w > 0.0f)
            {
                low = std::max(low, volumeLow[axis]);
                top = std::min(top, volumeHigh[axis]);
            }

            mark(bins[axis], counts[axis], i, low - BoxMargin - origin[axis], top + BoxMargin - origin[axis]);
        }
    }
}
}
