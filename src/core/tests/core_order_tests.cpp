#include "core/frame_latch.hpp"
#include "core/order.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using fxcore::DrawClaim;
using fxcore::FrameLatch;
using fxcore::Slot;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::string> calls;

bool recordsFirst(int&)
{
    calls.emplace_back("first");
    return false;
}

bool recordsSecond(int&)
{
    calls.emplace_back("second");
    return false;
}

bool recordsThird(int&)
{
    calls.emplace_back("third");
    return false;
}

bool takesTheWork(int&)
{
    calls.emplace_back("takes");
    return true;
}

void lowerPriorityRunsFirst()
{
    int work{};
    fxcore::Chain<int&> chain;
    calls.clear();

    expect(chain.add(20, "second", recordsSecond), "Add the later step.");
    expect(chain.add(10, "first", recordsFirst), "Add the earlier step.");
    expect(chain.run(work) == nullptr, "No step took the work.");
    expect(calls == std::vector<std::string>{"first", "second"}, "A lower priority runs first.");
}

void equalPrioritiesKeepRegistrationOrder()
{
    int work{};
    fxcore::Chain<int&> chain;
    calls.clear();

    chain.add(5, "first", recordsFirst);
    chain.add(5, "third", recordsThird);
    chain.add(5, "second", recordsSecond);
    chain.run(work);
    expect(calls == std::vector<std::string>{"first", "third", "second"}, "Equal priorities keep the order they were added in.");
}

void theFirstWinnerEndsTheList()
{
    int work{};
    fxcore::Chain<int&> chain;
    calls.clear();

    chain.add(10, "declines", recordsFirst);
    chain.add(20, "takes", takesTheWork);
    chain.add(30, "never", recordsSecond);
    expect(std::string(chain.run(work)) == "takes", "The step that took the work is named.");
    expect(calls == std::vector<std::string>{"first", "takes"}, "No step after the winner runs.");
}

void theSameStepIsAddedOnce()
{
    int work{};
    fxcore::Chain<int&> chain;
    calls.clear();

    expect(chain.add(10, "takes", takesTheWork), "Add a step.");
    expect(!chain.add(20, "takes again", takesTheWork), "The same step cannot be added twice.");
    chain.run(work);
    expect(calls == std::vector<std::string>{"takes"}, "The refused step does not run.");
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

void aFrameKeepsTheSettingsItStartedWith()
{
    FrameLatch<int> latch;

    latch.set(1);
    expect(latch.latest() == 1, "Latest holds the change.");
    expect(latch.frame() == 0, "No frame has started yet.");
    expect(latch.present() == 1, "The next frame starts with the latest settings.");

    latch.set(2);
    expect(latch.frame() == 1, "The frame being recorded keeps its settings.");
    expect(latch.present() == 2, "The frame after it takes the change.");
}
}

int main()
{
    try
    {
        lowerPriorityRunsFirst();
        equalPrioritiesKeepRegistrationOrder();
        theFirstWinnerEndsTheList();
        theSameStepIsAddedOnce();
        oneWinnerPerDraw();
        aFrameKeepsTheSettingsItStartedWith();
        std::cout << "Core order tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
