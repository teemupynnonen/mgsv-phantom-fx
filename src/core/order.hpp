#pragma once

#include <array>
#include <cstddef>
#include <vector>

// The order a frame's features run in: which part of the frame each works in, and which feature owns a
// draw. See docs/addon-plan.md.
namespace fxcore
{
// Where in a frame a feature works. A feature declares its slot; it cannot invent one.
enum class Slot
{
    FrameStart,
    GameDraw, // the game's draws, before lighting
    ShadersBound,
    SceneReady,
    BeforeUpscale,
    BeforePresent,
    Overlay,
};

inline constexpr std::size_t SlotCount = 7;

const char* label(Slot slot);

// One feature in one slot. A lower priority runs first; equal priorities keep the order they were
// added in, so a feature that only needs "before lighting" does not depend on registration order.
struct Step
{
    int priority{};
    const void* owner{};
};

class Schedule
{
public:
    // False when `owner` already holds a step in `slot`.
    bool add(Slot slot, int priority, const void* owner);
    const std::vector<Step>& ordered(Slot slot) const;

private:
    std::array<std::vector<Step>, SlotCount> slots;
};

// A draw belongs to one feature: the first claim takes it and a second is refused, which the caller
// logs. Clear at the start of every draw.
class DrawClaim
{
public:
    bool take(const void* owner);
    const void* owner() const;
    void clear();

private:
    const void* claimed{};
};
}
