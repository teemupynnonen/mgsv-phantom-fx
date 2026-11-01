#include "kernel_table.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace phantom_fx::kernel_table;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

BoundResource texture(const std::string& name, unsigned slot)
{
    return {name, D3D_SIT_TEXTURE, D3D_SRV_DIMENSION_TEXTURE2D, slot};
}

BoundResource output(const std::string& name, unsigned slot, unsigned count = 1)
{
    return {name, D3D_SIT_UAV_RWTYPED, D3D_SRV_DIMENSION_TEXTURE2D, slot, count};
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

bool fails(const std::vector<CompiledKernel>& kernels, const std::string& because)
{
    try
    {
        bindingsHeader(kernels);
    }
    catch (const std::exception& error)
    {
        return has(error.what(), because);
    }

    return false;
}

void eachKindGetsItsType()
{
    const auto text = bindingsHeader({{"Kernel",
        {texture("g_Depth", 0),
            {"g_Sky", D3D_SIT_TEXTURE, D3D_SRV_DIMENSION_TEXTURECUBE, 1},
            {"g_Records", D3D_SIT_STRUCTURED, D3D_SRV_DIMENSION_BUFFER, 2},
            {"g_Pool", D3D_SIT_BYTEADDRESS, D3D_SRV_DIMENSION_BUFFER, 3},
            output("g_Light", 0),
            {"g_Counters", D3D_SIT_UAV_RWSTRUCTURED, D3D_SRV_DIMENSION_BUFFER, 1},
            {"g_PoolOut", D3D_SIT_UAV_RWBYTEADDRESS, D3D_SRV_DIMENSION_BUFFER, 2}}}});

    expect(has(text, "TextureSlot Depth{0};"), "A 2D texture.");
    expect(has(text, "CubeSlot Sky{1};"), "A cube.");
    expect(has(text, "BufferSlot Records{2};"), "A structured buffer.");
    expect(has(text, "RawBufferSlot Pool{3};"), "A raw buffer.");
    expect(has(text, "OutputTextureSlot Light{0};"), "An output texture.");
    expect(has(text, "OutputBufferSlot Counters{1};"), "An output buffer.");
    expect(has(text, "OutputRawBufferSlot PoolOut{2};"), "An output raw buffer.");
}

void whatTheRootSignatureBindsIsLeftOut()
{
    const auto text = bindingsHeader({{"Kernel",
        {{"g_Frame", D3D_SIT_CBUFFER, D3D_SRV_DIMENSION_UNKNOWN, 0},
            {"g_Sampler", D3D_SIT_SAMPLER, D3D_SRV_DIMENSION_UNKNOWN, 0},
            {"g_Scene", D3D_SIT_RTACCELERATIONSTRUCTURE, D3D_SRV_DIMENSION_UNKNOWN, 0, 1, 1},
            {"g_SceneTextures", D3D_SIT_TEXTURE, D3D_SRV_DIMENSION_TEXTURE2D, 0, 4096, 2},
            texture("g_Depth", 0)}}});

    expect(has(text, "TextureSlot Depth{0};"), "The table's texture stays.");
    expect(!has(text, "Frame") && !has(text, "Sampler") && !has(text, "Scene"), "Constant buffers, samplers and other spaces have no slot of the tables.");
}

void kernelsShareASlotByName()
{
    const auto text = bindingsHeader({{"First", {texture("g_Depth", 0)}}, {"Second", {texture("g_Depth", 0), output("g_Light", 3)}}});
    expect(has(text, "TextureSlot Depth{0};") && has(text, "OutputTextureSlot Light{3};"), "One name in two kernels is one slot.");
    expect(has(text, "TextureSlots = 1, OutputSlots = 4"), "The tables reach as far as their last slot.");
}

void anArrayTakesItsSlots()
{
    const auto text = bindingsHeader({{"Kernel", {output("g_Noisy", 10, 2), output("g_Next", 12)}}});
    expect(has(text, "OutputTextureSlot Noisy{10, 2};"), "An array keeps its size.");
    expect(has(text, "OutputSlots = 13"), "And counts towards the tables' reach.");
}

void aSlotHasOneName()
{
    expect(fails({{"Kernel", {texture("g_Depth", 0), texture("g_Normal", 0)}}}, "share t0"), "Two names on one slot fail.");
    expect(fails({{"First", {output("g_A", 5)}}, {"Second", {output("g_B", 5)}}}, "share u5"), "Across kernels too.");
    expect(fails({{"Kernel", {output("g_Noisy", 10, 2), output("g_Next", 11)}}}, "share u11"), "A name on an array's second slot fails.");
    expect(!fails({{"Kernel", {texture("g_Depth", 0), output("g_Light", 0)}}}, "share"), "t0 and u0 are different slots.");
}

void aNameHasOneShape()
{
    expect(fails({{"First", {texture("g_Depth", 0)}}, {"Second", {texture("g_Depth", 1)}}}, "two shapes"), "Another slot in another kernel fails.");
    expect(fails({{"First", {texture("g_Depth", 0)}}, {"Second", {output("g_Depth", 0)}}}, "two shapes"), "So does another kind.");
    expect(fails({{"First", {output("g_Noisy", 10, 2)}}, {"Second", {output("g_Noisy", 10, 3)}}}, "two shapes"), "And another size.");
}

void whatTheTablesCannotHoldFails()
{
    expect(fails({{"Empty", {}}}, "no bindings"), "A kernel without bindings fails.");
    expect(fails({{"Kernel", {{"g_Volume", D3D_SIT_TEXTURE, D3D_SRV_DIMENSION_TEXTURE3D, 0}}}}, "no kind"), "A kind the tables have no view for fails.");
    expect(fails({{"Kernel", {texture("Depth", 0)}}}, "no g_ prefix"), "A name without the prefix fails.");
}

void kernelsKeepTheirOrder()
{
    const std::vector<std::string> kernels{"Second", "First"};

    const auto list = kernelListHeader(kernels);
    expect(list.find("Second,") < list.find("First,") && has(list, "Count"), "The enum keeps the list's order.");

    const auto bytecode = kernelBytecodeHeader(kernels);
    expect(bytecode.find("GI_Second") < bytecode.find("GI_First"), "The bytecode keeps the enum's order.");
    expect(has(bytecode, "#include \"First.h\""), "It includes each kernel.");
}
}

int main()
{
    try
    {
        eachKindGetsItsType();
        whatTheRootSignatureBindsIsLeftOut();
        kernelsShareASlotByName();
        anArrayTakesItsSlots();
        aSlotHasOneName();
        aNameHasOneShape();
        whatTheTablesCannotHoldFails();
        kernelsKeepTheirOrder();
        std::cout << "Kernel table tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
