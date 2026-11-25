#include "devkit/host.hpp"

#include <atomic>

namespace devkit
{
namespace
{
std::atomic<const char*> addonName{"add-on"};
std::atomic<Toast> sink{};
}

void setName(const char* name) noexcept
{
    addonName.store(name ? name : "add-on", std::memory_order_release);
}

std::string_view name() noexcept
{
    return addonName.load(std::memory_order_acquire);
}

void setToast(Toast toast) noexcept
{
    sink.store(toast, std::memory_order_release);
}

void toast(Tone tone, std::string_view text, std::string_view detail) noexcept
{
    if (const auto send = sink.load(std::memory_order_acquire)) send(tone, text, detail);
}
}
