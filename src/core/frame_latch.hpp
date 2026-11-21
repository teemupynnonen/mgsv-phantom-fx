#pragma once

#include <mutex>

namespace fxcore
{
// A module's settings as configure sets them, and as the frame being recorded uses them. Configure and
// the game's recording run on different threads; present starts the next frame with the latest
// settings, so no frame mixes two of them.
template<class T> class FrameLatch
{
public:
    void set(const T& value)
    {
        std::lock_guard lock(mutex);
        latestValue = value;
    }

    // What the next frame will use.
    T latest() const
    {
        std::lock_guard lock(mutex);
        return latestValue;
    }

    // At present: the next frame uses the latest settings, which this returns.
    T present()
    {
        std::lock_guard lock(mutex);
        frameValue = latestValue;
        return frameValue;
    }

    // What the frame being recorded uses.
    T frame() const
    {
        std::lock_guard lock(mutex);
        return frameValue;
    }

private:
    mutable std::mutex mutex;
    T latestValue{}, frameValue{};
};
}
