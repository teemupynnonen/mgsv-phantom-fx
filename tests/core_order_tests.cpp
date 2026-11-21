#include "core/order.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using fxcore::DrawClaim;
using fxcore::Schedule;
using fxcore::Slot;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<const void*> owners(const Schedule& schedule, Slot slot)
{
    std::vector<const void*> result;
    for (const auto& step : schedule.ordered(slot))
        result.push_back(step.owner);

    return result;
}

void lowerPriorityRunsFirst()
{
    int later{};
    int earlier{};
    Schedule schedule;

    expect(schedule.add(Slot::GameDraw, 20, &later), "Add the later step.");
    expect(schedule.add(Slot::GameDraw, 10, &earlier), "Add the earlier step.");
    expect(owners(schedule, Slot::GameDraw) == std::vector<const void*>{&earlier, &later}, "A lower priority runs first.");
}

void equalPrioritiesKeepRegistrationOrder()
{
    int first{};
    int second{};
    int third{};
    Schedule schedule;

    schedule.add(Slot::GameDraw, 5, &first);
    schedule.add(Slot::GameDraw, 5, &third);
    schedule.add(Slot::GameDraw, 5, &second);
    expect(owners(schedule, Slot::GameDraw) == std::vector<const void*>{&first, &third, &second}, "Equal priorities keep the order they were added in.");
}

void slotsAreIndependent()
{
    int draw{};
    int present{};
    Schedule schedule;

    schedule.add(Slot::GameDraw, 5, &draw);
    schedule.add(Slot::BeforePresent, 5, &present);
    expect(owners(schedule, Slot::GameDraw) == std::vector<const void*>{&draw}, "A step stays in its slot.");
    expect(owners(schedule, Slot::BeforePresent) == std::vector<const void*>{&present}, "Another slot holds its own steps.");
    expect(owners(schedule, Slot::SceneReady).empty(), "A slot nobody uses is empty.");
}

void aFeatureHoldsOneStepPerSlot()
{
    int feature{};
    Schedule schedule;

    expect(schedule.add(Slot::GameDraw, 5, &feature), "Add a feature.");
    expect(!schedule.add(Slot::GameDraw, 9, &feature), "The same feature cannot hold two steps in one slot.");
    expect(schedule.ordered(Slot::GameDraw).size() == 1, "The refused step was not added.");
    expect(schedule.add(Slot::Overlay, 5, &feature), "The same feature may work in another slot.");
}

void oneWinnerPerDraw()
{
    int first{};
    int second{};
    DrawClaim claim;

    expect(claim.take(&first), "The first claim takes the draw.");
    expect(claim.take(&first), "The owner may claim its own draw again.");
    expect(!claim.take(&second), "A second feature is refused.");
    expect(claim.owner() == &first, "The first claim stays the owner.");
    expect(!claim.take(nullptr), "Nothing cannot claim a draw.");

    claim.clear();
    expect(claim.take(&second), "The next draw is free.");
}
}

int main()
{
    try
    {
        lowerPriorityRunsFirst();
        equalPrioritiesKeepRegistrationOrder();
        slotsAreIndependent();
        aFeatureHoldsOneStepPerSlot();
        oneWinnerPerDraw();
        std::cout << "Core order tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
