#pragma once

#if !defined(PHANTOM_FX_DEV_TOOLS) || !PHANTOM_FX_DEV_TOOLS
#error "hot_kernels.hpp is for development builds: use the dev preset."
#endif

#include <cstddef>
#include <cstdint>
#include <vector>

// The ray tracing's Direct3D 12 kernels, reloaded as their sources are saved, for development. A worker thread watches
// each kernel's .hlsl and the files every kernel includes (CMakeLists.txt's GI_SHADER_INCLUDES); when one changes it
// compiles the kernels it touches with the build's DXC, as the build does, and publishes their DXIL. The renderer takes
// it at the start of a frame (gi_renderer.cpp) and makes the pipelines there.
//
// A failed compile is logged and the kernel in use stays. Only edits to a kernel's body reload: its bindings are the
// build's (tools/kernel_bindings), so a new or moved register still needs a rebuild.
namespace phantom_fx::hot_kernels
{
struct Compiled
{
    std::size_t kernel{}; // gi_d3d12::Kernel's value
    std::vector<std::uint8_t> code;
};

// Starts the watcher, once; later calls do nothing.
void start() noexcept;
// Stops it and joins its thread, clearing the way for the add-on to unload: a jthread joined while the loader lock is
// held can hang on the thread's own exit. AddonUninit calls this.
void stop() noexcept;
// What compiled since the last call.
std::vector<Compiled> take() noexcept;
// Every kernel's last good compile, for a renderer that has just started: it begins from the build's DXIL, and what was
// taken before it is not taken again. Saves made before the add-on started are compiled when the watcher starts.
std::vector<Compiled> replay() noexcept;
}
