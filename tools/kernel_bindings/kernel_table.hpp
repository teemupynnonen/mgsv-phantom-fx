#pragma once

#include <d3dcommon.h>
#include <string>
#include <vector>

// What the build makes of the compiled kernels. Their register() lines are the one home of their slots, and CMakeLists.txt's
// add_gi_kernels of the kernels themselves; C++ gets both from here (docs/DEVELOPMENT.md, "Kernels and their slots").
namespace phantom_fx::kernel_table
{
struct BoundResource
{
    std::string name;
    D3D_SHADER_INPUT_TYPE type{};
    D3D_SRV_DIMENSION dimension{};
    unsigned slot{}, count{1}, space{};
};

struct CompiledKernel
{
    std::string name;
    std::vector<BoundResource> resources;
};

// gi_bindings.h: every slot of the frame's descriptor tables, by name. Throws when a kernel has no bindings, a slot has two
// names, a name has two shapes, or a resource is of no kind the tables know.
std::string bindingsHeader(const std::vector<CompiledKernel>& kernels);
// gi_kernel_list.h: the Kernel enum.
std::string kernelListHeader(const std::vector<std::string>& kernels);
// gi_kernel_bytecode.h: each kernel's DXIL, in the enum's order.
std::string kernelBytecodeHeader(const std::vector<std::string>& kernels);
}
