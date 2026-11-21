#include "core/order.hpp"

namespace fxcore
{
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
