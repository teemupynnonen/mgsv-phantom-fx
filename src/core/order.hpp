#pragma once

#include <algorithm>
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

// What runs in one slot, in priority order. Equal priorities keep the order they were added in, so a
// feature that only needs "before lighting" does not depend on registration order. The first step that
// takes the work ends the list: a draw is drawn in the game's place exactly once.
template<class... Args> class Chain
{
public:
    using Run = bool (*)(Args...);

    // False when `run` already holds a step.
    bool add(int priority, const char* name, Run run)
    {
        for (const auto& step : steps)
            if (step.run == run) return false;

        // After every equal priority: registration order decides between equals.
        const auto at = std::upper_bound(steps.begin(), steps.end(), priority, [](int value, const Step& step) { return value < step.priority; });
        steps.insert(at, Step{priority, name, run});

        return true;
    }

    // Runs the steps in order until one takes the work. Returns the winner's name, or nullptr.
    const char* run(Args... args) const
    {
        for (const auto& step : steps)
            if (step.run(args...)) return step.name;

        return nullptr;
    }

    bool empty() const
    {
        return steps.empty();
    }

private:
    struct Step
    {
        int priority;
        const char* name;
        Run run;
    };

    std::vector<Step> steps;
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
