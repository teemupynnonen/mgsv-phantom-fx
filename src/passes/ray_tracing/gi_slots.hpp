#pragma once

#include <stdexcept>

// The slots of the frame's descriptor tables. Their numbers are the register() lines of the kernels' HLSL: the build gives
// C++ each one by name (gi_bindings.h; docs/DEVELOPMENT.md, "Kernels and their slots").
namespace phantom_fx::binding
{
enum class SlotKind { Texture, Cube, Buffer, RawBuffer, OutputTexture, OutputBuffer, OutputRawBuffer };

// Each kind a type of its own, so a view of the wrong kind does not compile.
template<SlotKind Kind> struct Slot
{
    unsigned index{};
    unsigned count{1};

    // An element of an array.
    consteval Slot at(unsigned element) const
    {
        if (element >= count) throw std::out_of_range("Past the end of the array.");

        return {index + element, 1};
    }
};

using TextureSlot = Slot<SlotKind::Texture>;
using CubeSlot = Slot<SlotKind::Cube>;
using BufferSlot = Slot<SlotKind::Buffer>;
using RawBufferSlot = Slot<SlotKind::RawBuffer>;
using OutputTextureSlot = Slot<SlotKind::OutputTexture>;
using OutputBufferSlot = Slot<SlotKind::OutputBuffer>;
using OutputRawBufferSlot = Slot<SlotKind::OutputRawBuffer>;
}
