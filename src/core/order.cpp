#include "core/order.hpp"

#include <algorithm>

namespace fxcore
{
namespace
{
std::size_t indexOf(Slot slot)
{
    return static_cast<std::size_t>(slot);
}
}

const char* label(Slot slot)
{
    switch (slot)
    {
    case Slot::FrameStart: return "frame start";
    case Slot::GameDraw: return "game draw";
    case Slot::ShadersBound: return "shaders bound";
    case Slot::SceneReady: return "scene ready";
    case Slot::BeforeUpscale: return "before upscale";
    case Slot::BeforePresent: return "before present";
    case Slot::Overlay: return "overlay";
    }

    return "unknown";
}

bool Schedule::add(Slot slot, int priority, const void* owner)
{
    auto& steps = slots[indexOf(slot)];
    for (const auto& step : steps)
        if (step.owner == owner) return false;

    // After every equal priority: registration order decides between equals.
    const auto at = std::upper_bound(steps.begin(), steps.end(), priority, [](int value, const Step& step) { return value < step.priority; });
    steps.insert(at, Step{priority, owner});

    return true;
}

const std::vector<Step>& Schedule::ordered(Slot slot) const
{
    return slots[indexOf(slot)];
}

bool DrawClaim::take(const void* owner)
{
    if (!owner) return false;
    if (claimed) return claimed == owner;

    claimed = owner;

    return true;
}

const void* DrawClaim::owner() const
{
    return claimed;
}

void DrawClaim::clear()
{
    claimed = nullptr;
}
}
