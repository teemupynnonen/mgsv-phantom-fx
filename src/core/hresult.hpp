#pragma once

#include "log.hpp"

#include <Windows.h>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace phantom_fx
{
// Throws when a Direct3D or Windows call failed, naming the step and its code.
inline void check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string(what) + " failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
}
}
