#include "frame_log.hpp"

#include "fxui/fxui.hpp"
#include "log.hpp"
#include "resource_dump.hpp"
#include "shader_bytecode.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <reshade.hpp>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace phantom_fx::frame_log
{
namespace
{
using namespace reshade::api;

constexpr UINT ResourceSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr UINT ConstantSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
constexpr UINT OutputSlots = D3D11_PS_CS_UAV_REGISTER_COUNT; // Feature level 11.0
constexpr UINT TargetSlots = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
// WKPDID_D3DDebugObjectName, defined here so dxguid.lib is not needed.
constexpr GUID DebugObjectName{0x429b8c22, 0x9188, 0x4b0c, {0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00}};

// A frame is recorded after one warm-up frame: the game records command lists on deferred
// contexts ahead of executing them, and the warm-up catches lists executed early in the next frame.
enum class Capture { Off, WarmingUp, Recording };

// The PhantomFX-frame-log folder, which turns the frame log on and receives its files.
std::filesystem::path outputDirectory;
bool active{}, saveShaders{};

std::mutex shaderMutex;
std::unordered_map<std::uint64_t, std::string> shaderHashes;
std::unordered_set<std::string> loggedShaders;

std::atomic<Capture> capture{Capture::Off};
std::atomic<bool> requested{};

// A texture a dumped call had bound, with a reference held until it is saved.
struct Texture
{
    std::string label;
    ID3D11Resource* resource{};
    DXGI_FORMAT format{};
};

// A vertex buffer slot past the first, copied from its bound offset: an instanced draw's per-instance
// data is in one of them (the clone shaders' matrix rows, TEXCOORD2 to 4).
struct Stream
{
    UINT slot{}, stride{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> data;
};

// What a dumped indexed draw drew, copied into staging buffers on the context that drew it, in its
// order: the draw's indices, the vertex buffer from its bound offset, and the other bound slots. The
// light passes' volumes are clip-space vertices the engine writes each frame, so only this shows their
// shape.
struct Vertices
{
    Microsoft::WRL::ComPtr<ID3D11Buffer> indices, vertices;
    UINT indexSize{}, stride{}, count{};
    INT base{};
    std::vector<Stream> streams;
    UINT instances{}, firstInstance{};
};

struct Line
{
    std::string text;
    std::vector<Texture> textures;
    Vertices vertices;
};

// What dump.txt asks of a shader's next calls: their constants, and unless "constants" follows the
// count, their textures too.
struct DumpRequest
{
    int left{};
    bool textures{};
};
enum class Dump { None, Constants, Everything };

// Guarded by recordMutex.
std::mutex recordMutex;
std::ofstream record;
std::filesystem::path dumpDirectory;
std::string recordName;
std::uint64_t presents{}, calls{};
command_list* immediate{};
ID3D11Resource* backBuffer{};
// Lines from deferred contexts and the command lists they finish into, until the immediate
// context executes them, so the record is in execution order.
std::unordered_map<command_list*, std::vector<Line>> pending;
// Textures of dumped calls in executed command lists, saved at the immediate context's next event,
// when the GPU has run the list: inputs as the call read them, outputs as the list left them.
std::vector<std::pair<std::uint64_t, Texture>> queuedTextures;
std::vector<std::pair<std::uint64_t, Vertices>> queuedVertices;
// From dump.txt: shaders whose next calls also dump their constants, vertices and textures.
std::unordered_map<std::string, DumpRequest> dumpsLeft;
// What the game last wrote to each constant buffer, per thread: each deferred context records
// on its own thread and sees its own copy of a dynamic buffer.
using ThreadBuffer = std::pair<DWORD, std::uint64_t>;
std::map<ThreadBuffer, std::vector<std::uint8_t>> constantContents;
std::map<ThreadBuffer, const void*> mappedConstants;
// Who maps constant buffers for writing, by buffer size and call stack, with a count: shows where
// the engine fills, for example, the camera constants.
std::map<std::string, unsigned> constantWriters;
// From trigger.txt: a shader hash. F10 then records frame after frame, each over the last, until a
// frame runs a call with that shader; that frame is kept, and the one before it as `-before.txt`.
std::string trigger;
bool triggered{};
// Whether this capture's frame before the trigger is in frame-previous.txt, from restartRecording.
bool hasPrevious{};
// The frame whose record is in frame-previous.txt, whose dump folder lasts while it may still be needed.
std::string previousName;

// The caller's stack as "module+offset" frames, outermost last.
std::string callStack()
{
    void* frames[14]{};
    const auto count = CaptureStackBackTrace(2, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    std::string text;
    for (USHORT i = 0; i < count; ++i)
    {
        HMODULE module{};
        wchar_t path[MAX_PATH]{};
        char frame[96]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(frames[i]), &module) && GetModuleFileNameW(module, path, MAX_PATH))
            std::snprintf(frame, sizeof(frame), " %ls+0x%llX", std::filesystem::path(path).filename().c_str(),
                static_cast<unsigned long long>(static_cast<std::uint8_t*>(frames[i]) - reinterpret_cast<std::uint8_t*>(module)));
        else
            std::snprintf(frame, sizeof(frame), " %p", frames[i]);

        text += frame;
    }

    return text;
}

// References a context's Get call returns, released at the end of the scope.
template<class T, std::size_t Count> struct Bound
{
    std::array<T*, Count> objects{};

    Bound() = default;
    Bound(const Bound&) = delete;
    Bound& operator=(const Bound&) = delete;
    ~Bound()
    {
        for (auto* object : objects)
            if (object) object->Release();
    }

    T** data() { return objects.data(); }
    static constexpr UINT size() { return static_cast<UINT>(Count); }
};

ID3D11DeviceContext* native(command_list* list)
{
    return reinterpret_cast<ID3D11DeviceContext*>(list->get_native());
}

const char* stageName(pipeline_subobject_type type)
{
    switch (type)
    {
    case pipeline_subobject_type::vertex_shader: return "vs";
    case pipeline_subobject_type::hull_shader: return "hs";
    case pipeline_subobject_type::domain_shader: return "ds";
    case pipeline_subobject_type::geometry_shader: return "gs";
    case pipeline_subobject_type::pixel_shader: return "ps";
    case pipeline_subobject_type::compute_shader: return "cs";
    default: return nullptr;
    }
}

std::string pointer(const void* object)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "@%llX", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(object)));
    return text;
}

const char* comparisonName(D3D11_COMPARISON_FUNC function)
{
    constexpr const char* Names[]{"?", "never", "less", "equal", "less_equal", "greater", "not_equal", "greater_equal", "always"};
    return static_cast<std::size_t>(function) < std::size(Names) ? Names[function] : "?";
}

UINT bindFlags(ID3D11Resource* resource)
{
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;

    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);

    return desc.BindFlags;
}

// "@<resource> <format> <size>", plus the resource's own format when the view reinterprets it,
// the back buffer mark and the engine's debug name if it set one.
std::string describe(ID3D11Resource* resource, DXGI_FORMAT viewFormat)
{
    auto text = pointer(resource) + " ";
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
    {
        D3D11_BUFFER_DESC desc{};
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
        text += "buffer " + std::to_string(desc.ByteWidth);
        if (viewFormat != DXGI_FORMAT_UNKNOWN) text += " " + formatName(viewFormat);
    }
    else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
        text += formatName(viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : desc.Format);
        if (viewFormat != DXGI_FORMAT_UNKNOWN && viewFormat != desc.Format) text += " (" + formatName(desc.Format) + ")";
        text += " " + std::to_string(desc.Width) + "x" + std::to_string(desc.Height);
        if (desc.ArraySize > 1) text += " [" + std::to_string(desc.ArraySize) + "]";
        if (desc.MipLevels > 1) text += " mips " + std::to_string(desc.MipLevels);
        if (desc.SampleDesc.Count > 1) text += " msaa " + std::to_string(desc.SampleDesc.Count);
    }
    else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
    {
        D3D11_TEXTURE3D_DESC desc{};
        static_cast<ID3D11Texture3D*>(resource)->GetDesc(&desc);
        text += formatName(viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : desc.Format) + " " + std::to_string(desc.Width) + "x"
            + std::to_string(desc.Height) + "x" + std::to_string(desc.Depth) + " 3d";
    }
    else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
    {
        D3D11_TEXTURE1D_DESC desc{};
        static_cast<ID3D11Texture1D*>(resource)->GetDesc(&desc);
        text += formatName(viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : desc.Format) + " " + std::to_string(desc.Width) + " 1d";
    }

    if (resource == backBuffer) text += " BACKBUFFER";

    char name[128]{};
    UINT size = sizeof(name) - 1;
    if (SUCCEEDED(resource->GetPrivateData(DebugObjectName, &size, name)) && size) text += " \"" + std::string(name, size) + "\"";

    return text;
}

// A view's resource (with a reference the caller releases) and format.
using Viewed = std::pair<ID3D11Resource*, DXGI_FORMAT>;

template<class View, class Desc> Viewed viewed(View* view)
{
    ID3D11Resource* resource{};
    view->GetResource(&resource);
    Desc desc{};
    view->GetDesc(&desc);
    return {resource, desc.Format};
}

Viewed viewed(ID3D11ShaderResourceView* view) { return viewed<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(view); }
Viewed viewed(ID3D11RenderTargetView* view) { return viewed<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(view); }
Viewed viewed(ID3D11DepthStencilView* view) { return viewed<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(view); }
Viewed viewed(ID3D11UnorderedAccessView* view) { return viewed<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>(view); }

Viewed viewed(ID3D11Buffer* buffer)
{
    buffer->AddRef();
    return {buffer, DXGI_FORMAT_UNKNOWN};
}

std::string describe(resource resource)
{
    return resource.handle ? describe(reinterpret_cast<ID3D11Resource*>(resource.handle), DXGI_FORMAT_UNKNOWN) : "null";
}

template<class View> std::string describeView(View* view)
{
    const auto [resource, format] = viewed(view);
    auto text = describe(resource, format);
    resource->Release();
    return text;
}

// Appends " | <label> <slot>0 <view>, <slot>3 <view>" for the bound objects.
template<class Object, std::size_t Count> void appendBound(std::string& line, const char* label, const char* slot, Bound<Object, Count>& bound)
{
    std::string text;
    for (std::size_t i = 0; i < Count; ++i)
    {
        if (!bound.objects[i]) continue;

        if (!text.empty()) text += ", ";
        text += slot + std::to_string(i) + " " + describeView(bound.objects[i]);
    }

    if (!text.empty()) line += std::string(" | ") + label + " " + text;
}

template<class Shader> std::string hashOf(Shader* shader)
{
    if (!shader) return "";

    std::string hash;
    {
        std::lock_guard lock(shaderMutex);
        const auto found = shaderHashes.find(reinterpret_cast<std::uint64_t>(shader));
        hash = found != shaderHashes.end() ? found->second : "unknown" + pointer(shader);
    }

    shader->Release();

    return hash;
}

std::string viewport(ID3D11DeviceContext* context)
{
    UINT count = 1;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&count, &viewport);
    if (!count) return "";

    char text[96]{};
    std::snprintf(text, sizeof(text), " | vp %g,%g %gx%g", viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height);
    std::string result = text;
    if (viewport.MinDepth != 0 || viewport.MaxDepth != 1)
    {
        std::snprintf(text, sizeof(text), " z %g-%g", viewport.MinDepth, viewport.MaxDepth);
        result += text;
    }

    return result;
}

std::string outputStates(ID3D11DeviceContext* context)
{
    std::string text = " | blend ";
    ID3D11BlendState* blend{};
    float factor[4]{};
    UINT mask{};
    context->OMGetBlendState(&blend, factor, &mask);
    if (blend)
    {
        D3D11_BLEND_DESC desc{};
        blend->GetDesc(&desc);
        blend->Release();
        const auto& target = desc.RenderTarget[0];
        text += target.BlendEnable ? std::to_string(target.SrcBlend) + "/" + std::to_string(target.DestBlend) + " op "
                + std::to_string(target.BlendOp) : "off";
        if (target.RenderTargetWriteMask != D3D11_COLOR_WRITE_ENABLE_ALL) text += " mask " + std::to_string(target.RenderTargetWriteMask);
    }
    else text += "off";

    text += " | depth ";
    ID3D11DepthStencilState* depth{};
    UINT stencilReference{};
    context->OMGetDepthStencilState(&depth, &stencilReference);
    D3D11_DEPTH_STENCIL_DESC desc{TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS};
    if (depth)
    {
        depth->GetDesc(&desc);
        depth->Release();
    }

    text += desc.DepthEnable ? comparisonName(desc.DepthFunc) : "off";
    if (desc.DepthEnable && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) text += " write";
    if (desc.StencilEnable) text += " stencil ref " + std::to_string(stencilReference);

    // Which faces the draw culls, and which winding is its front (without a state: back faces, clockwise).
    ID3D11RasterizerState* rasterizer{};
    context->RSGetState(&rasterizer);
    D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, D3D11_CULL_BACK};
    if (rasterizer)
    {
        rasterizer->GetDesc(&raster);
        rasterizer->Release();
    }

    text += raster.CullMode == D3D11_CULL_NONE ? " | cull none" : raster.CullMode == D3D11_CULL_FRONT ? " | cull front" : " | cull back";
    if (raster.FrontCounterClockwise) text += " ccw";

    return text;
}

void release(std::vector<Texture>& textures)
{
    for (auto& texture : textures)
        texture.resource->Release();

    textures.clear();
}

void save(std::uint64_t call, const Texture& texture)
{
    std::error_code error;
    std::filesystem::create_directories(dumpDirectory, error);
    const auto file = dumpDirectory / (std::to_string(call) + "-" + texture.label + ".dds");
    if (!saveTexture(native(immediate), texture.resource, texture.format, file)) log("Could not save " + file.filename().string() + ".");
    texture.resource->Release();
}

// Reads a staging buffer; waits for the GPU.
std::vector<std::uint8_t> read(ID3D11Buffer* staging)
{
    D3D11_BUFFER_DESC desc{};
    staging->GetDesc(&desc);
    std::vector<std::uint8_t> contents;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(native(immediate)->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        const auto* data = static_cast<const std::uint8_t*>(mapped.pData);
        contents.assign(data, data + desc.ByteWidth);
        native(immediate)->Unmap(staging, 0);
    }

    return contents;
}

// <call>-vertices.txt: each index's vertex as its first four floats (the light volumes' clip-space
// position), then the bounds in normalized device coordinates of those in front of the eye.
void save(std::uint64_t call, const Vertices& drawn)
{
    const auto indices = read(drawn.indices.Get()), vertices = read(drawn.vertices.Get());
    std::error_code error;
    std::filesystem::create_directories(dumpDirectory, error);
    std::ofstream file(dumpDirectory / (std::to_string(call) + "-vertices.txt"), std::ios::trunc);
    float low[2]{1e30f, 1e30f}, high[2]{-1e30f, -1e30f};
    unsigned behind{};
    for (UINT i = 0; i < drawn.count && (i + 1) * drawn.indexSize <= indices.size(); ++i)
    {
        std::uint32_t index{};
        std::memcpy(&index, indices.data() + i * drawn.indexSize, drawn.indexSize);
        const auto offset = (static_cast<std::int64_t>(drawn.base) + index) * drawn.stride;
        file << i << ": index " << index;
        if (offset < 0 || offset + 16 > static_cast<std::int64_t>(vertices.size()))
        {
            file << " (past the copied vertices)\n";
            continue;
        }

        float position[4]{};
        std::memcpy(position, vertices.data() + offset, sizeof(position));
        file << " " << position[0] << " " << position[1] << " " << position[2] << " " << position[3] << '\n';
        if (position[3] <= 0)
        {
            ++behind;
            continue;
        }

        for (int axis = 0; axis < 2; ++axis)
        {
            low[axis] = std::min(low[axis], position[axis] / position[3]);
            high[axis] = std::max(high[axis], position[axis] / position[3]);
        }
    }

    file << "in front: x " << low[0] << " to " << high[0] << ", y " << low[1] << " to " << high[1] << "; " << behind << " behind the eye\n";
    if (drawn.streams.empty()) return;

    // <call>-streams.txt: each other slot's elements from its bound offset as floats, a line each.
    std::ofstream streams(dumpDirectory / (std::to_string(call) + "-streams.txt"), std::ios::trunc);
    streams << drawn.instances << " instances from " << drawn.firstInstance << '\n';
    for (const auto& stream : drawn.streams)
    {
        const auto bytes = read(stream.data.Get());
        const std::size_t rows = bytes.size() / stream.stride;
        streams << "slot " << stream.slot << ", stride " << stream.stride << ", " << rows << " elements\n";
        for (std::size_t row = 0; row < rows; ++row)
        {
            streams << row << ':';
            for (UINT at = 0; at + 4 <= stream.stride; at += 4)
            {
                float value{};
                std::memcpy(&value, bytes.data() + row * stream.stride + at, 4);
                streams << ' ' << value;
            }

            streams << '\n';
        }
    }
}

// Saves what executed command lists left; call with recordMutex held, on the immediate context.
void saveQueued()
{
    for (const auto& [call, texture] : queuedTextures)
        save(call, texture);

    for (const auto& [call, vertices] : queuedVertices)
        save(call, vertices);

    queuedTextures.clear();
    queuedVertices.clear();
}

// Called with recordMutex held, for each line written to the record.
void noteTrigger(const std::string& text)
{
    if (!trigger.empty() && text.find(trigger) != std::string::npos) triggered = true;
}

// Writes one line of the record, call number and all, which is what a trigger copied from a record
// line is matched against. Called with recordMutex held.
void writeRecord(std::string text)
{
    record << text << '\n';
    noteTrigger(text);
}

// Writes a line in execution order: the immediate context's as they come, a command list's when
// the immediate context executes it. A call on the immediate context saves its textures at once,
// as they are before it runs.
void emit(command_list* list, Line line)
{
    std::lock_guard lock(recordMutex);
    if (list != immediate)
    {
        pending[list].push_back(std::move(line));
        return;
    }

    if (capture != Capture::Recording)
    {
        release(line.textures);
        return;
    }

    writeRecord("#" + std::to_string(++calls) + " " + line.text);
    for (const auto& texture : line.textures)
        save(calls, texture);

    if (line.vertices.indices) save(calls, line.vertices);
}

void emit(command_list* list, std::string text)
{
    emit(list, Line{std::move(text), {}, {}});
}

// What of this call dump.txt asks for, counting it off.
Dump takeDump(std::initializer_list<const std::string*> hashes)
{
    if (capture != Capture::Recording) return Dump::None;

    std::lock_guard lock(recordMutex);
    for (const auto* hash : hashes)
    {
        const auto found = dumpsLeft.find(*hash);
        if (found != dumpsLeft.end() && found->second.left > 0)
        {
            --found->second.left;
            return found->second.textures ? Dump::Everything : Dump::Constants;
        }
    }

    return Dump::None;
}

// Copies `size` bytes of `source` from `offset` into a new staging buffer, on `context` in its order.
Microsoft::WRL::ComPtr<ID3D11Buffer> copyToStaging(ID3D11DeviceContext* context, ID3D11Buffer* source, UINT offset, UINT size)
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    const D3D11_BUFFER_DESC desc{size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
    Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
    if (!size || FAILED(device->CreateBuffer(&desc, nullptr, staging.GetAddressOf()))) return nullptr;

    const D3D11_BOX box{offset, 0, 0, offset + size, 1, 1};
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, 0, &box);

    return staging;
}

// The indexed draw's indices and up to 64 KB of its vertices from where they are bound.
Vertices copyVertices(ID3D11DeviceContext* context, UINT count, UINT first, INT base)
{
    constexpr UINT MostVertexBytes = 64 << 10;
    Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer, vertexBuffer;
    DXGI_FORMAT indexFormat{};
    UINT indexOffset{}, stride{}, vertexOffset{};
    context->IAGetIndexBuffer(indexBuffer.GetAddressOf(), &indexFormat, &indexOffset);
    context->IAGetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &vertexOffset);
    if (!indexBuffer || !vertexBuffer) return {};

    D3D11_BUFFER_DESC indexDesc{}, vertexDesc{};
    indexBuffer->GetDesc(&indexDesc);
    vertexBuffer->GetDesc(&vertexDesc);
    const UINT indexSize = indexFormat == DXGI_FORMAT_R32_UINT ? 4 : 2;
    const UINT indexStart = indexOffset + first * indexSize;
    if (indexStart + count * indexSize > indexDesc.ByteWidth || vertexOffset >= vertexDesc.ByteWidth) return {};

    Vertices drawn{copyToStaging(context, indexBuffer.Get(), indexStart, count * indexSize),
        copyToStaging(context, vertexBuffer.Get(), vertexOffset, std::min(MostVertexBytes, vertexDesc.ByteWidth - vertexOffset)), indexSize, stride,
        count, base};
    if (!drawn.indices || !drawn.vertices) return {};

    for (UINT slot = 1; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++slot)
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        UINT slotStride{}, slotOffset{};
        context->IAGetVertexBuffers(slot, 1, buffer.GetAddressOf(), &slotStride, &slotOffset);
        if (!buffer || !slotStride) continue;

        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        if (slotOffset >= desc.ByteWidth) continue;

        if (auto copy = copyToStaging(context, buffer.Get(), slotOffset, std::min(MostVertexBytes, desc.ByteWidth - slotOffset)))
            drawn.streams.push_back({slot, slotStride, std::move(copy)});
    }

    return drawn;
}

// The contents of the bound constant buffers as the game last wrote them on this thread. Buffers
// written before the capture are read back when on the immediate context.
template<std::size_t Count>
std::string constantsOf(const char* stage, Bound<ID3D11Buffer, Count>& buffers, ID3D11DeviceContext* context)
{
    std::string text;
    for (std::size_t slot = 0; slot < Count; ++slot)
    {
        auto* buffer = buffers.objects[slot];
        if (!buffer) continue;

        text += std::string("\n  ") + stage + " b" + std::to_string(slot) + " " + pointer(buffer);
        std::vector<std::uint8_t> contents;
        {
            std::lock_guard lock(recordMutex);
            const auto found = constantContents.find({GetCurrentThreadId(), reinterpret_cast<std::uint64_t>(buffer)});
            if (found != constantContents.end()) contents = found->second;
        }

        if (contents.empty() && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            if (const auto staging = copyToStaging(context, buffer, 0, desc.ByteWidth)) contents = read(staging.Get());
            text += " (read back)";
        }

        text += contents.empty() ? " (not written this frame)\n" : "\n" + constantRows(contents.data(), contents.size());
    }

    return text;
}

template<class View, std::size_t Count> void collectTextures(std::vector<Texture>& textures, const char* label, Bound<View, Count>& views)
{
    for (std::size_t slot = 0; slot < Count; ++slot)
    {
        if (!views.objects[slot]) continue;

        const auto [resource, format] = viewed(views.objects[slot]);
        textures.push_back({label + std::to_string(slot), resource, format});
    }
}

// An indexed draw's indices: how many, the first, and the base vertex added to each.
struct IndexRange
{
    UINT count{}, first{};
    INT base{};
    UINT instances{}, firstInstance{};
};

// One line per call: what it is, then the shaders and every bound input and output. Calls named
// in dump.txt add their constants and save their textures and, for an indexed draw, its vertices.
void recordCall(command_list* list, std::string line, bool compute, const IndexRange* indexed = nullptr)
{
    auto* context = native(list);
    Bound<ID3D11ShaderResourceView, ResourceSlots> resources;
    Bound<ID3D11UnorderedAccessView, OutputSlots> outputs;
    Bound<ID3D11Buffer, ConstantSlots> vertexConstants, constants;
    Bound<ID3D11RenderTargetView, TargetSlots> targets;
    Bound<ID3D11DepthStencilView, 1> depth;
    std::string vertexHash, pixelHash, computeHash;
    const char* stage = compute ? "cs" : "ps";

    if (compute)
    {
        ID3D11ComputeShader* shader{};
        context->CSGetShader(&shader, nullptr, nullptr);
        computeHash = hashOf(shader);
        context->CSGetShaderResources(0, resources.size(), resources.data());
        context->CSGetUnorderedAccessViews(0, outputs.size(), outputs.data());
        context->CSGetConstantBuffers(0, constants.size(), constants.data());
        line += " | cs " + computeHash;
    }
    else
    {
        ID3D11VertexShader* vertexShader{};
        context->VSGetShader(&vertexShader, nullptr, nullptr);
        vertexHash = hashOf(vertexShader);
        ID3D11GeometryShader* geometryShader{};
        context->GSGetShader(&geometryShader, nullptr, nullptr);
        const auto geometryHash = hashOf(geometryShader);
        ID3D11PixelShader* pixelShader{};
        context->PSGetShader(&pixelShader, nullptr, nullptr);
        pixelHash = hashOf(pixelShader);
        context->OMGetRenderTargets(targets.size(), targets.data(), depth.data());
        context->PSGetShaderResources(0, resources.size(), resources.data());
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, outputs.size(), outputs.data());
        context->VSGetConstantBuffers(0, vertexConstants.size(), vertexConstants.data());
        context->PSGetConstantBuffers(0, constants.size(), constants.data());

        line += " | vs " + (vertexHash.empty() ? "-" : vertexHash);
        if (!geometryHash.empty()) line += " gs " + geometryHash;
        line += " ps " + (pixelHash.empty() ? "-" : pixelHash);
        appendBound(line, "om", "rt", targets);
        appendBound(line, "ds", "", depth);
        line += viewport(context) + outputStates(context);
    }

    appendBound(line, stage, "t", resources);
    appendBound(line, stage, "u", outputs);
    appendBound(line, "vs cb", "b", vertexConstants);
    appendBound(line, compute ? "cs cb" : "ps cb", "b", constants);

    Line entry{std::move(line), {}, {}};
    const auto dump = takeDump({&vertexHash, &pixelHash, &computeHash});
    if (dump != Dump::None)
    {
        entry.text += constantsOf("vs", vertexConstants, context) + constantsOf(stage, constants, context);
        if (indexed)
        {
            entry.vertices = copyVertices(context, indexed->count, indexed->first, indexed->base);
            entry.vertices.instances = indexed->instances;
            entry.vertices.firstInstance = indexed->firstInstance;
        }
    }

    if (dump == Dump::Everything)
    {
        collectTextures(entry.textures, compute ? "cs-t" : "ps-t", resources);
        collectTextures(entry.textures, compute ? "cs-u" : "ps-u", outputs);
        collectTextures(entry.textures, "rt", targets);
        collectTextures(entry.textures, "ds", depth);
    }

    emit(list, std::move(entry));
}

// dump.txt: a line per shader, "<hash> <calls>", with " constants" after to leave out the textures.
std::unordered_map<std::string, DumpRequest> readDumpList()
{
    std::unordered_map<std::string, DumpRequest> list;
    std::ifstream file(outputDirectory / "dump.txt");
    std::string text;
    while (std::getline(file, text))
    {
        std::istringstream fields(text);
        std::string hash, only;
        int count{};
        if (!(fields >> hash >> count) || hash[0] == '#') continue;

        fields >> only;
        list[hash] = {count, only != "constants"};
    }

    return list;
}

void saveBytecode(const char* stage, const std::string& hash, const shader_desc& shader)
{
    std::ofstream file(outputDirectory / "shaders" / (std::string(stage) + "_" + hash + ".dxbc"), std::ios::binary);
    file.write(static_cast<const char*>(shader.code), static_cast<std::streamsize>(shader.code_size));
}

void onInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipeline)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* stage = stageName(subobjects[i].type);
        const auto* shader = static_cast<const shader_desc*>(subobjects[i].data);
        if (!stage || !shader) continue;

        const auto checksum = dxbcHashOf(shader->code, shader->code_size);
        const auto hash = checksum ? toHex(*checksum) : "not-dxbc";
        std::lock_guard lock(shaderMutex);
        shaderHashes[pipeline.handle] = hash;
        if (!loggedShaders.insert(std::string(stage) + hash).second) continue;

        log(std::string("Shader ") + stage + " " + hash + ", " + std::to_string(shader->code_size) + " bytes.");
        if (saveShaders && checksum) saveBytecode(stage, hash, *shader);
    }
}

void onDestroyPipeline(device*, pipeline pipeline)
{
    std::lock_guard lock(shaderMutex);
    shaderHashes.erase(pipeline.handle);
}

// trigger.txt's line without its ends' spaces, or nothing without the file. Any text of a record line
// will do, spaces and all ("depth greater_equal"), so the whole line is read, not its first word.
std::string readTrigger()
{
    std::ifstream file(outputDirectory / "trigger.txt");
    std::string text;
    std::getline(file, text);

    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");

    return text.substr(first, last - first + 1);
}

std::filesystem::path previousRecord()
{
    return outputDirectory / "frame-previous.txt";
}

// A frame's dump folder, thrown away with the frame it belonged to: the lights and the scene dump into
// it without asking dump.txt, so a wait of many frames would otherwise leave one folder a frame.
void removeDumpDirectory(const std::string& name)
{
    if (name.empty()) return;

    std::error_code error;
    std::filesystem::remove_all(outputDirectory / name, error);
}

// While waiting for the trigger: the frame just recorded becomes the previous one, and the next starts.
void restartRecording()
{
    std::lock_guard lock(recordMutex);
    saveQueued();
    record.close();
    const auto recorded = outputDirectory / (recordName + ".txt");
    std::error_code error;
    // The frame before, which frame-previous.txt holds, is replaced by this one: its folder goes with it.
    removeDumpDirectory(previousName);
    previousName = recordName;
    // The frame before the trigger is the one right before it, so the copy kept is replaced each time.
    std::filesystem::remove(previousRecord(), error);
    std::filesystem::rename(recorded, previousRecord(), error);
    hasPrevious = !error;
    if (error) std::filesystem::remove(recorded, error);
    recordName = "frame-" + std::to_string(presents);
    record = std::ofstream(outputDirectory / (recordName + ".txt"), std::ios::trunc);
    dumpDirectory = outputDirectory / recordName;
    calls = 0;
    constantWriters.clear();
}

void finishRecording()
{
    std::lock_guard lock(recordMutex);
    saveQueued();
    record.close();
    log("Recorded " + std::to_string(calls) + " calls" + (trigger.empty() || !triggered ? "." : ", the first frame to run " + trigger + "."));
    if (!trigger.empty())
    {
        // Only a frame this capture recorded, so an interrupted capture's file never stands in.
        const auto before = outputDirectory / (recordName + "-before.txt");
        std::error_code error;
        if (hasPrevious)
        {
            std::filesystem::rename(previousRecord(), before, error);
            if (error) log("Could not keep the frame before " + recordName + ": " + error.message());
        }
        // The frame's number can come round again after the game restarts, and an earlier run's file
        // beside this capture would pass for its predecessor.
        else
            std::filesystem::remove(before, error);

        trigger.clear();
        hasPrevious = false;
        previousName.clear();
    }

    record.open(outputDirectory / (recordName + "-writers.txt"), std::ios::trunc);
    for (const auto& [writer, count] : constantWriters)
        record << count << "x " << writer << '\n';

    record.close();
    fxui::toast(fxui::Tone::Ok, "Frame captured", recordName + ".txt");
    constantWriters.clear();
    for (auto& [list, lines] : pending)
        for (auto& line : lines)
            release(line.textures);

    pending.clear();
    constantContents.clear();
    mappedConstants.clear();
    capture = Capture::Off;
}

void onPresent(command_queue* queue, swapchain* chain, const rect*, const rect*, uint32_t, const rect*)
{
    {
        std::lock_guard lock(recordMutex);
        ++presents;
        immediate = queue->get_immediate_command_list();
        backBuffer = reinterpret_cast<ID3D11Resource*>(chain->get_current_back_buffer().handle);
    }

    if (capture == Capture::Recording)
    {
        bool dumping{}, waiting{};
        {
            std::lock_guard lock(recordMutex);
            // The frame's last line, written before the wait decides: a frame a wait turns over keeps
            // it too, and a trigger on this line finds the frame it was written for.
            writeRecord("#" + std::to_string(++calls) + " present " + describe(backBuffer, DXGI_FORMAT_UNKNOWN));
            dumping = !dumpsLeft.empty();
            waiting = !trigger.empty() && !triggered;
        }

        // F10 again stops waiting and keeps the frame; pressed as the trigger matches, it stops the
        // capture rather than starting another one.
        const bool stop = requested.exchange(false);
        if (waiting && !stop)
        {
            restartRecording();
            return;
        }


        // The frame's final image, since the passes after FXAA write the back buffer.
        if (dumping) saveTexture(native(immediate), backBuffer, DXGI_FORMAT_UNKNOWN, dumpDirectory / "present.dds");
        finishRecording();
    }
    else if (capture == Capture::WarmingUp)
    {
        std::lock_guard lock(recordMutex);
        recordName = "frame-" + std::to_string(presents);
        record = std::ofstream(outputDirectory / (recordName + ".txt"), std::ios::trunc);
        dumpDirectory = outputDirectory / recordName;
        calls = 0;
        capture = Capture::Recording;
        log("Recording " + recordName + ".txt" + (dumpsLeft.empty() ? "." : ", with dumps in " + recordName + "/."));
    }
    else if (requested.exchange(false))
    {
        std::lock_guard lock(recordMutex);
        dumpsLeft = readDumpList();
        trigger = readTrigger();
        triggered = false;
        hasPrevious = false;
        previousName.clear();
        // A capture that was cut short leaves its frame before the trigger behind; it is not this one's.
        std::error_code error;
        std::filesystem::remove(previousRecord(), error);
        if (!trigger.empty()) fxui::toast(fxui::Tone::Ok, "Recording until a frame runs", trigger);
        capture = Capture::WarmingUp;
    }
}

void onReshadePresent(effect_runtime* runtime)
{
    if (runtime->is_key_pressed(VK_F10)) requested = true;
}

bool onDraw(command_list* list, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t firstInstance)
{
    if (capture != Capture::Off)
        recordCall(list, "draw " + std::to_string(vertices) + " x" + std::to_string(instances) + " from " + std::to_string(first)
            + (firstInstance ? " instance " + std::to_string(firstInstance) : ""), false);

    return false;
}

bool onDrawIndexed(command_list* list, uint32_t indices, uint32_t instances, uint32_t first, int32_t offset, uint32_t firstInstance)
{
    if (capture != Capture::Off)
    {
        const IndexRange range{indices, first, offset, instances, firstInstance};
        recordCall(list, "draw_indexed " + std::to_string(indices) + " x" + std::to_string(instances) + " from " + std::to_string(first)
            + " base " + std::to_string(offset) + (firstInstance ? " instance " + std::to_string(firstInstance) : ""), false, &range);
    }

    return false;
}

bool onDispatch(command_list* list, uint32_t x, uint32_t y, uint32_t z)
{
    if (capture != Capture::Off) recordCall(list, "dispatch " + std::to_string(x) + "x" + std::to_string(y) + "x" + std::to_string(z), true);

    return false;
}

bool onIndirect(command_list* list, indirect_command type, resource buffer, uint64_t offset, uint32_t, uint32_t)
{
    if (capture != Capture::Off)
        recordCall(list, (type == indirect_command::dispatch ? "dispatch_indirect " : "draw_indirect ") + describe(buffer) + " offset "
            + std::to_string(offset), type == indirect_command::dispatch);

    return false;
}

bool onClearRenderTarget(command_list* list, resource_view view, const float color[4], uint32_t, const rect*)
{
    if (capture == Capture::Off) return false;

    char values[96]{};
    std::snprintf(values, sizeof(values), " to %g %g %g %g", color[0], color[1], color[2], color[3]);
    emit(list, "clear_rtv " + describeView(reinterpret_cast<ID3D11RenderTargetView*>(view.handle)) + values);

    return false;
}

bool onClearDepthStencil(command_list* list, resource_view view, const float* depth, const uint8_t* stencil, uint32_t, const rect*)
{
    if (capture == Capture::Off) return false;

    auto line = "clear_dsv " + describeView(reinterpret_cast<ID3D11DepthStencilView*>(view.handle));
    if (depth) line += " depth " + std::to_string(*depth);
    if (stencil) line += " stencil " + std::to_string(*stencil);
    emit(list, line);

    return false;
}

bool onClearUnorderedAccessUint(command_list* list, resource_view view, const uint32_t values[4], uint32_t, const rect*)
{
    if (capture != Capture::Off)
        emit(list, "clear_uav " + describeView(reinterpret_cast<ID3D11UnorderedAccessView*>(view.handle)) + " to " + std::to_string(values[0]));

    return false;
}

bool onClearUnorderedAccessFloat(command_list* list, resource_view view, const float values[4], uint32_t, const rect*)
{
    if (capture != Capture::Off)
        emit(list, "clear_uav " + describeView(reinterpret_cast<ID3D11UnorderedAccessView*>(view.handle)) + " to " + std::to_string(values[0]));

    return false;
}

bool onCopyResource(command_list* list, resource source, resource dest)
{
    if (capture != Capture::Off) emit(list, "copy_resource " + describe(source) + " -> " + describe(dest));

    return false;
}

bool onCopyBufferRegion(command_list* list, resource source, uint64_t sourceOffset, resource dest, uint64_t destOffset, uint64_t size)
{
    if (capture != Capture::Off)
        emit(list, "copy_buffer_region " + describe(source) + " +" + std::to_string(sourceOffset) + " -> " + describe(dest) + " +"
            + std::to_string(destOffset) + " size " + std::to_string(size));

    return false;
}

bool onCopyTextureRegion(command_list* list, resource source, uint32_t sourceSubresource, const subresource_box* sourceBox, resource dest,
    uint32_t destSubresource, const subresource_box*, filter_mode)
{
    if (capture == Capture::Off) return false;

    auto line = "copy_texture_region " + describe(source) + " sub " + std::to_string(sourceSubresource);
    if (sourceBox)
        line += " box " + std::to_string(sourceBox->left) + "," + std::to_string(sourceBox->top) + " " + std::to_string(sourceBox->width())
            + "x" + std::to_string(sourceBox->height());

    emit(list, line + " -> " + describe(dest) + " sub " + std::to_string(destSubresource));

    return false;
}

bool onResolveTextureRegion(command_list* list, resource source, uint32_t, const subresource_box*, resource dest, uint32_t, uint32_t,
    uint32_t, uint32_t, format)
{
    if (capture != Capture::Off) emit(list, "resolve " + describe(source) + " -> " + describe(dest));

    return false;
}

bool onUpdateTextureRegion(command_list* list, const subresource_data&, resource dest, uint32_t subresource, const subresource_box*)
{
    if (capture != Capture::Off) emit(list, "update_texture " + describe(dest) + " sub " + std::to_string(subresource));

    return false;
}

bool onGenerateMipmaps(command_list* list, resource_view view)
{
    if (capture != Capture::Off) emit(list, "generate_mips " + describeView(reinterpret_cast<ID3D11ShaderResourceView*>(view.handle)));

    return false;
}

// Constant buffer writes, kept so dumps can show what a call read.
void onMapBuffer(device*, resource buffer, uint64_t, uint64_t, map_access access, void** data)
{
    if (capture == Capture::Off || access == map_access::read_only || !data || !*data
        || !(bindFlags(reinterpret_cast<ID3D11Resource*>(buffer.handle)) & D3D11_BIND_CONSTANT_BUFFER))
        return;

    D3D11_BUFFER_DESC desc{};
    reinterpret_cast<ID3D11Buffer*>(buffer.handle)->GetDesc(&desc);
    auto writer = "size " + std::to_string(desc.ByteWidth) + " <-" + callStack();
    std::lock_guard lock(recordMutex);
    mappedConstants[{GetCurrentThreadId(), buffer.handle}] = *data;
    ++constantWriters[std::move(writer)];
}

void onUnmapBuffer(device*, resource buffer)
{
    if (capture == Capture::Off) return;

    std::lock_guard lock(recordMutex);
    const auto found = mappedConstants.find({GetCurrentThreadId(), buffer.handle});
    if (found == mappedConstants.end()) return;

    D3D11_BUFFER_DESC desc{};
    reinterpret_cast<ID3D11Buffer*>(buffer.handle)->GetDesc(&desc);
    const auto* data = static_cast<const std::uint8_t*>(found->second);
    constantContents[found->first].assign(data, data + desc.ByteWidth);
    mappedConstants.erase(found);
}

bool onUpdateBufferRegion(command_list*, const void* data, resource dest, uint64_t offset, uint64_t size)
{
    if (capture == Capture::Off || !(bindFlags(reinterpret_cast<ID3D11Resource*>(dest.handle)) & D3D11_BIND_CONSTANT_BUFFER)) return false;

    std::lock_guard lock(recordMutex);
    auto& contents = constantContents[{GetCurrentThreadId(), dest.handle}];
    if (contents.size() < offset + size) contents.resize(offset + size);
    std::memcpy(contents.data() + offset, data, size);

    return false;
}

// At FinishCommandList the deferred context's lines move to the new command list; at
// ExecuteCommandList the command list's lines are written where the immediate context runs them.
void onExecuteSecondary(command_list* list, command_list* secondary)
{
    if (capture == Capture::Off) return;

    std::lock_guard lock(recordMutex);
    auto lines = std::move(pending[secondary]);
    pending.erase(secondary);

    if (list != immediate)
    {
        auto& target = pending[list];
        target.insert(target.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
        return;
    }

    if (capture != Capture::Recording)
    {
        for (auto& line : lines)
            release(line.textures);

        return;
    }

    saveQueued();
    writeRecord("#" + std::to_string(++calls) + " execute_command_list " + pointer(secondary)
        + (lines.empty() ? " (recorded before the capture)" : ""));

    for (auto& line : lines)
    {
        writeRecord("#" + std::to_string(++calls) + "   " + line.text);
        for (auto& texture : line.textures)
            queuedTextures.emplace_back(calls, std::move(texture));

        if (line.vertices.indices) queuedVertices.emplace_back(calls, std::move(line.vertices));
    }
}

void onDestroyCommandList(command_list* list)
{
    std::lock_guard lock(recordMutex);
    const auto found = pending.find(list);
    if (found == pending.end()) return;

    for (auto& line : found->second)
        release(line.textures);

    pending.erase(found);
}

template<reshade::addon_event Event> void subscribe(bool on, typename reshade::addon_event_traits<Event>::decl callback)
{
    if (on) reshade::register_event<Event>(callback);
    else reshade::unregister_event<Event>(callback);
}

// One list for both, so what install registers is what uninstall removes.
void subscribe(bool on)
{
    subscribe<reshade::addon_event::init_pipeline>(on, onInitPipeline);
    subscribe<reshade::addon_event::destroy_pipeline>(on, onDestroyPipeline);
    subscribe<reshade::addon_event::present>(on, onPresent);
    subscribe<reshade::addon_event::reshade_present>(on, onReshadePresent);
    subscribe<reshade::addon_event::draw>(on, onDraw);
    subscribe<reshade::addon_event::draw_indexed>(on, onDrawIndexed);
    subscribe<reshade::addon_event::dispatch>(on, onDispatch);
    subscribe<reshade::addon_event::draw_or_dispatch_indirect>(on, onIndirect);
    subscribe<reshade::addon_event::clear_render_target_view>(on, onClearRenderTarget);
    subscribe<reshade::addon_event::clear_depth_stencil_view>(on, onClearDepthStencil);
    subscribe<reshade::addon_event::clear_unordered_access_view_uint>(on, onClearUnorderedAccessUint);
    subscribe<reshade::addon_event::clear_unordered_access_view_float>(on, onClearUnorderedAccessFloat);
    subscribe<reshade::addon_event::copy_resource>(on, onCopyResource);
    subscribe<reshade::addon_event::copy_buffer_region>(on, onCopyBufferRegion);
    subscribe<reshade::addon_event::copy_texture_region>(on, onCopyTextureRegion);
    subscribe<reshade::addon_event::resolve_texture_region>(on, onResolveTextureRegion);
    subscribe<reshade::addon_event::update_texture_region_command>(on, onUpdateTextureRegion);
    subscribe<reshade::addon_event::generate_mipmaps>(on, onGenerateMipmaps);
    subscribe<reshade::addon_event::map_buffer_region>(on, onMapBuffer);
    subscribe<reshade::addon_event::unmap_buffer_region>(on, onUnmapBuffer);
    subscribe<reshade::addon_event::update_buffer_region_command>(on, onUpdateBufferRegion);
    subscribe<reshade::addon_event::execute_secondary_command_list>(on, onExecuteSecondary);
    subscribe<reshade::addon_event::destroy_command_list>(on, onDestroyCommandList);
}
}

void install(const std::filesystem::path& addonDirectory)
{
    outputDirectory = addonDirectory / "PhantomFX-frame-log";
    std::error_code error;
    if (!std::filesystem::is_directory(outputDirectory, error)) return;

    active = true;
    saveShaders = std::filesystem::is_directory(outputDirectory / "shaders", error);
    log("Frame log on: F10 records a frame into PhantomFX-frame-log.");

    subscribe(true);
}

bool installed()
{
    return active;
}

std::filesystem::path recordingDirectory()
{
    if (capture != Capture::Recording) return {};

    std::lock_guard lock(recordMutex);
    std::error_code error;
    std::filesystem::create_directories(dumpDirectory, error);

    return dumpDirectory;
}

void uninstall()
{
    if (!active) return;

    active = false;
    subscribe(false);

    std::lock_guard lock(recordMutex);
    record.close();
    pending.clear();
    capture = Capture::Off;
}
}
