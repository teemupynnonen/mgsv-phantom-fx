#include "devkit/log.hpp"

#include <atomic>

namespace devkit
{
namespace
{
std::atomic<Logger> sink{};
}

void setLogger(Logger logger) noexcept
{
    sink.store(logger, std::memory_order_release);
}

void log(std::string_view message) noexcept
{
    if (const auto logger = sink.load(std::memory_order_acquire)) logger(message);
}
}
