#include "dev/held_camera.hpp"

#include <mutex>

namespace phantom_fx::held_camera
{
namespace
{
struct Motion
{
    Pose from, to;
    int step{}, steps{};
};

std::mutex poseMutex;
std::optional<Pose> heldPose, renderedPose;
std::optional<Motion> motion;

Pose between(const Pose& from, const Pose& to, float t)
{
    Pose pose;
    for (int i = 0; i < 3; ++i) pose.position[i] = from.position[i] + (to.position[i] - from.position[i]) * t;
    pose.yaw = from.yaw + (to.yaw - from.yaw) * t;
    pose.pitch = from.pitch + (to.pitch - from.pitch) * t;

    return pose;
}
}

void hold(const Pose& pose) noexcept
{
    std::scoped_lock lock(poseMutex);
    heldPose = pose;
    motion.reset();
}

void move(const Pose& pose, int updates) noexcept
{
    std::scoped_lock lock(poseMutex);
    const auto from = heldPose ? *heldPose : renderedPose.value_or(pose);
    if (updates > 0) motion = Motion{from, pose, 0, updates};
    else heldPose = pose;
}

void release() noexcept
{
    std::scoped_lock lock(poseMutex);
    heldPose.reset();
    motion.reset();
}

bool isHeld() noexcept
{
    std::scoped_lock lock(poseMutex);

    return heldPose.has_value();
}

std::optional<Pose> lastCamera() noexcept
{
    std::scoped_lock lock(poseMutex);

    return renderedPose;
}

std::optional<Pose> next() noexcept
{
    std::scoped_lock lock(poseMutex);
    if (motion)
    {
        heldPose = between(motion->from, motion->to, static_cast<float>(++motion->step) / static_cast<float>(motion->steps));
        if (motion->step >= motion->steps) motion.reset();
    }

    return heldPose;
}

void remember(const Pose& pose) noexcept
{
    std::scoped_lock lock(poseMutex);
    renderedPose = pose;
}
}
