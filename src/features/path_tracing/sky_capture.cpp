// The sky as the rays see it; sky_capture.hpp describes it.
#include "features/path_tracing/sky_capture.hpp"

#include "constant_capture.hpp"
#include "d3d11_helpers.hpp"
#include "game_constants.hpp"
#include "log.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "performance.hpp"

#include <Windows.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string>

namespace phantom_fx::sky_capture
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_constants::ObjectConstants;
using game_constants::SceneConstants;
using game_constants::SystemConstants;

constexpr UINT FaceSize = 256, Faces = 6;
constexpr DXGI_FORMAT Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
// The constant buffers the sky draws read at their vertex and pixel shaders, as the frame logs list them.
constexpr UINT SystemSlot = 0, SceneSlot = 2, LightSlot = 3, ObjectSlot = 5, WorkSlot = 7, Slots = 8;
// Without a sky draw for this long (a hospital's corridors) the rays take the sky's light.
constexpr std::uint64_t SkyGoneMilliseconds = 2000;
// The most a texel gives, in the exposed units the game's white is 1 of: in SDR the sun's sprite is far above.
constexpr float MostSky = 4;

// A cube face's camera, as Direct3D's cube maps have them: where its right, up and forward go in the world.
struct FaceAxes
{
    std::array<float, 3> right, up, forward;
};

constexpr std::array<FaceAxes, Faces> FaceCameras{{
    {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}},
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
    {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},
    {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
    {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
}};

// cVSLight, b3 (176 bytes), at the sky draw's vertex shader: c0.x viewFar, which only scales its ray; c1 clipPlaneOfView.
struct SkyLightConstants
{
    float viewFar[4];
    float clipPlaneOfView[4];
    float unused[9][4];
};

// cVSWork, b7 (576 bytes), at the sky draw's vertex shader: m_viewInverse, which it does not read, then m_matrix[0] clip
// to view at c4 and m_matrix[1] view to world at c8.
struct SkyWorkConstants
{
    float viewInverse[4][4];
    float clipToView[4][4];
    float viewToWorld[4][4];
    float unused[6][4][4];
};

static_assert(sizeof(SkyLightConstants) == 176 && sizeof(SkyWorkConstants) == 576);

// The face's texture holds what the scene would: exposed, and sRGB encoded beyond 1 (docs/DEVELOPMENT.md, "HDR").
constexpr char ToRayUnits[] = R"(
Texture2DArray<float4> Drawn : register(t0);
RWTexture2DArray<float4> Output : register(u0);

cbuffer Parameters : register(b0)
{
    uint Face;
    uint Size;
    float Scale; // From the exposed light to the rays' units
    float Most;
};

float3 Decoded(float3 encoded)
{
    float3 high = pow((max(encoded, 0) + 0.055) / 1.055, 2.4);
    return encoded <= 0.04045 ? encoded / 12.92 : high;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= Size)) return;

    Output[uint3(id, Face)] = float4(clamp(Decoded(Drawn.Load(int4(id, Face, 0)).rgb), 0, Most) * Scale, 1);
}
)";

struct Parameters
{
    std::uint32_t face, size;
    float scale, most;
};

std::atomic<bool> wanted{};

// Everything below with resourceMutex held.
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
bool pipelineFailed{};
ComPtr<ID3D11Texture2D> drawn;
ComPtr<ID3D11ShaderResourceView> drawnView;
std::array<ComPtr<ID3D11RenderTargetView>, Faces> faceTargets;
ComPtr<ID3D11Texture2D> cube;
ComPtr<ID3D11UnorderedAccessView> cubeAccess;
ComPtr<ID3D11RasterizerState> rasterizer;
ComPtr<ID3D11Buffer> systemBuffer, sceneBuffer, lightBuffer, objectBuffer, workBuffer, parameterBuffer;
ComPtr<ID3D11ComputeShader> toRayUnits;

// The face this frame's draws go into, and whether one did; the face the last frame drew and its exposure, for update.
std::uint32_t currentFace{};
bool isDrawing{};
float currentExposure{};
std::optional<std::uint32_t> drawnFace;
float drawnExposure{};
std::uint32_t readyFaces{}; // A bit each
std::uint64_t lastDraw{};

bool ensurePipeline()
{
    if (cube) return true;
    if (pipelineFailed || !device) return false;

    try
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = FaceSize;
        desc.MipLevels = 1;
        desc.ArraySize = Faces;
        desc.Format = Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&desc, nullptr, drawn.ReleaseAndGetAddressOf()), "Creating the sky's faces");
        check(device->CreateShaderResourceView(drawn.Get(), nullptr, drawnView.ReleaseAndGetAddressOf()), "Creating the sky's faces' view");
        for (UINT face = 0; face < Faces; ++face)
        {
            D3D11_RENDER_TARGET_VIEW_DESC target{};
            target.Format = Format;
            target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            target.Texture2DArray.FirstArraySlice = face;
            target.Texture2DArray.ArraySize = 1;
            check(device->CreateRenderTargetView(drawn.Get(), &target, faceTargets[face].ReleaseAndGetAddressOf()), "Creating a sky face's target");
        }

        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = SharedWithD3D12;
        ComPtr<ID3D11Texture2D> shared;
        check(device->CreateTexture2D(&desc, nullptr, shared.GetAddressOf()), "Creating the sky's cube");
        check(device->CreateUnorderedAccessView(shared.Get(), nullptr, cubeAccess.ReleaseAndGetAddressOf()), "Creating the sky's cube's view");

        // Whichever way a face's camera turns the dome; every ray lands on the sky, however far the game's dome lies.
        D3D11_RASTERIZER_DESC state{};
        state.FillMode = D3D11_FILL_SOLID;
        state.CullMode = D3D11_CULL_NONE;
        state.DepthClipEnable = FALSE;
        check(device->CreateRasterizerState(&state, rasterizer.ReleaseAndGetAddressOf()), "Creating the sky's rasterizer state");

        systemBuffer = makeConstantBuffer(device.Get(), sizeof(SystemConstants), "Creating the sky's system constants");
        sceneBuffer = makeConstantBuffer(device.Get(), sizeof(SceneConstants), "Creating the sky's scene constants");
        lightBuffer = makeConstantBuffer(device.Get(), sizeof(SkyLightConstants), "Creating the sky's light constants");
        objectBuffer = makeConstantBuffer(device.Get(), sizeof(ObjectConstants), "Creating the sky's object constants");
        workBuffer = makeConstantBuffer(device.Get(), sizeof(SkyWorkConstants), "Creating the sky's work constants");
        parameterBuffer = makeConstantBuffer(device.Get(), sizeof(Parameters), "Creating the sky's parameters");
        toRayUnits = computeShader(device.Get(), ToRayUnits, "sky-to-ray-units");
        cube = shared;
        log("Path tracing: the sky is captured for the rays.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        cube.Reset();
        log(std::string("Path tracing: the rays keep the sky's light: ") + error.what());

        return false;
    }
}

float dot3(const std::array<float, 3>& a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The product of 4 x 4 matrices of rows, as the engine's are applied.
void multiply(const float left[4][4], const float right[4][4], float result[4][4])
{
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            result[row][column] = 0;
            for (int k = 0; k < 4; ++k) result[row][column] += left[row][k] * right[k][column];
        }
}

// The scene constants with the camera at the same eye, turned to the face, a right angle across and without the
// upscaler's jitter, and a face's size.
SceneConstants turnedTo(const SceneConstants& scene, const FaceAxes& face)
{
    SceneConstants turned = scene;
    const float eye[3]{scene.eye[0], scene.eye[1], scene.eye[2]};
    const auto setRow = [&](float* row, const std::array<float, 3>& axis) {
        std::memcpy(row, axis.data(), sizeof(float) * 3);
        row[3] = -dot3(axis, eye);
    };

    setRow(turned.view[0], face.right);
    setRow(turned.view[1], face.up);
    setRow(turned.view[2], face.forward);
    const float across[2][4]{{1, 0, 0, 0}, {0, 1, 0, 0}};
    std::memcpy(turned.projection, across, sizeof(across));
    turned.projection[2][0] = turned.projection[2][1] = 0;
    multiply(turned.projection, turned.view, turned.projectionView);
    turned.projectionParam[0] = turned.projectionParam[1] = 1;
    turned.width = turned.height = static_cast<float>(FaceSize);
    std::memset(turned.cameraCenterOffset, 0, sizeof(turned.cameraCenterOffset));

    return turned;
}

// A dome's object constants under the turned camera: its object to view takes the game's view out and the face's in.
ObjectConstants turnedTo(const ObjectConstants& object, const SceneConstants& scene, const SceneConstants& turned)
{
    // The game's view is a rotation and a move: its inverse is its transpose and the move back.
    float viewToWorld[4][4]{};
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column) viewToWorld[row][column] = scene.view[column][row];
        viewToWorld[row][3] = -(scene.view[0][row] * scene.view[0][3] + scene.view[1][row] * scene.view[1][3] + scene.view[2][row] * scene.view[2][3]);
    }

    viewToWorld[3][3] = 1;
    float faceFromGame[4][4]{};
    multiply(turned.view, viewToWorld, faceFromGame);
    ObjectConstants result = object;
    multiply(faceFromGame, object.viewWorld, result.viewWorld);

    return result;
}

SkyWorkConstants workFor(const FaceAxes& face)
{
    SkyWorkConstants work{};
    work.clipToView[0][0] = work.clipToView[1][1] = work.clipToView[2][3] = 1;
    for (int row = 0; row < 3; ++row)
    {
        work.viewToWorld[row][0] = face.right[row];
        work.viewToWorld[row][1] = face.up[row];
        work.viewToWorld[row][2] = face.forward[row];
    }

    return work;
}

// What a sky draw has bound of what capture changes, put back when this goes out of scope.
class SavedBindings
{
public:
    explicit SavedBindings(ID3D11DeviceContext* context) : context(context)
    {
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets.data(), depth.GetAddressOf());
        viewportCount = static_cast<UINT>(viewports.size());
        context->RSGetViewports(&viewportCount, viewports.data());
        context->RSGetState(rasterizerState.GetAddressOf());
        context->VSGetConstantBuffers(0, Slots, vertexBuffers.data());
        context->PSGetConstantBuffers(0, Slots, pixelBuffers.data());
    }

    ~SavedBindings()
    {
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets.data(), depth.Get());
        context->RSSetViewports(viewportCount, viewports.data());
        context->RSSetState(rasterizerState.Get());
        context->VSSetConstantBuffers(0, Slots, vertexBuffers.data());
        context->PSSetConstantBuffers(0, Slots, pixelBuffers.data());
        for (auto* target : targets)
            if (target) target->Release();
        for (auto* buffer : vertexBuffers)
            if (buffer) buffer->Release();
        for (auto* buffer : pixelBuffers)
            if (buffer) buffer->Release();
    }

    SavedBindings(const SavedBindings&) = delete;
    SavedBindings& operator=(const SavedBindings&) = delete;

    ID3D11Buffer* pixelBuffer(UINT slot) const { return pixelBuffers[slot]; }

private:
    ID3D11DeviceContext* context;
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    ComPtr<ID3D11DepthStencilView> depth;
    UINT viewportCount{};
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    ComPtr<ID3D11RasterizerState> rasterizerState;
    std::array<ID3D11Buffer*, Slots> vertexBuffers{}, pixelBuffers{};
};
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
}

void detach() noexcept
{
    wanted = false;
    std::lock_guard lock(resourceMutex);
    drawn.Reset();
    drawnView.Reset();
    faceTargets = {};
    cube.Reset();
    cubeAccess.Reset();
    rasterizer.Reset();
    systemBuffer.Reset();
    sceneBuffer.Reset();
    lightBuffer.Reset();
    objectBuffer.Reset();
    workBuffer.Reset();
    parameterBuffer.Reset();
    toRayUnits.Reset();
    device.Reset();
    pipelineFailed = false;
    readyFaces = 0;
    drawnFace.reset();
}

void configure(const Configuration& configuration) noexcept
{
    wanted = configuration.effects.runs(Effect::PathTracing);
}

void present() noexcept
{
    std::lock_guard lock(resourceMutex);
    if (isDrawing)
    {
        drawnFace = currentFace;
        drawnExposure = currentExposure;
        currentFace = (currentFace + 1) % Faces;
        isDrawing = false;
    }
}

bool isWanted() noexcept
{
    return wanted.load(std::memory_order_relaxed) && ray_tracing::isPathTracing();
}

void capture(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    // A smaller camera's view (the intro's mirror) is not the main view's.
    if (!isWanted() || !viewportFillsTarget(context)) return;

    try
    {
        const auto scene = constant_capture::pixelShaderReads<SceneConstants>(context);
        if (!scene || !(scene->exposure > 0)) return;

        const auto object = constant_capture::pixelShaderReads<ObjectConstants>(context);

        std::lock_guard lock(resourceMutex);
        if (!ensurePipeline()) return;

        const auto& face = FaceCameras[currentFace];
        const auto turned = turnedTo(*scene, face);
        SystemConstants system{};
        system.renderInfo[0] = system.renderInfo[1] = system.renderBuffer[0] = system.renderBuffer[1] = static_cast<float>(FaceSize);
        system.renderBuffer[2] = system.renderBuffer[3] = 1.0f / FaceSize;
        SkyLightConstants light{};
        light.viewFar[0] = 1000;
        light.clipPlaneOfView[3] = 1; // Every ray is above it: the game's sky bends those under its plane, in the view's space
        check(write(context, systemBuffer.Get(), system), "Writing the sky's system constants");
        check(write(context, sceneBuffer.Get(), turned), "Writing the sky's scene constants");
        check(write(context, lightBuffer.Get(), light), "Writing the sky's light constants");
        check(write(context, workBuffer.Get(), workFor(face)), "Writing the sky's work constants");
        if (object) check(write(context, objectBuffer.Get(), turnedTo(*object, *scene, turned)), "Writing the sky's object constants");

        const SavedBindings saved(context);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(FaceSize), static_cast<float>(FaceSize), 0, 1};
        ID3D11RenderTargetView* const target = faceTargets[currentFace].Get();
        context->OMSetRenderTargets(1, &target, nullptr);
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer.Get());
        const auto setBoth = [&](UINT slot, ID3D11Buffer* buffer) {
            context->VSSetConstantBuffers(slot, 1, &buffer);
            context->PSSetConstantBuffers(slot, 1, &buffer);
        };

        setBoth(SystemSlot, systemBuffer.Get());
        setBoth(SceneSlot, sceneBuffer.Get());
        setBoth(LightSlot, lightBuffer.Get());
        setBoth(WorkSlot, workBuffer.Get());
        if (object) setBoth(ObjectSlot, objectBuffer.Get());

        const performance::Timed timed(context, performance::Pass::PathTracedLight);
        if (call.indexed) context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
        else context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);

        isDrawing = true;
        currentExposure = scene->exposure;
        lastDraw = GetTickCount64();
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        pipelineFailed = true;
        cube.Reset();
        log(std::string("Path tracing: the rays keep the sky's light: ") + error.what());
    }
    catch (...)
    {
    }
}

ID3D11Texture2D* update(ID3D11DeviceContext* immediate, float lightScale) noexcept
{
    try
    {
        std::lock_guard lock(resourceMutex);
        if (!cube) return nullptr;

        if (drawnFace)
        {
            const SavedComputeState saved(immediate, ComputeSlots{.buffers = 1, .views = 1, .accesses = 1});
            const Parameters parameters{*drawnFace, FaceSize, lightScale / drawnExposure, MostSky};
            check(write(immediate, parameterBuffer.Get(), parameters), "Writing the sky's parameters");
            immediate->CSSetShader(toRayUnits.Get(), nullptr, 0);
            immediate->CSSetConstantBuffers(0, 1, parameterBuffer.GetAddressOf());
            immediate->CSSetShaderResources(0, 1, drawnView.GetAddressOf());
            immediate->CSSetUnorderedAccessViews(0, 1, cubeAccess.GetAddressOf(), nullptr);
            immediate->Dispatch(groups(FaceSize), groups(FaceSize), 1);
            readyFaces |= 1u << *drawnFace;
            drawnFace.reset();
        }

        const bool isWhole = readyFaces == (1u << Faces) - 1;
        return isWhole && GetTickCount64() - lastDraw < SkyGoneMilliseconds ? cube.Get() : nullptr;
    }
    catch (const std::exception& error)
    {
        log(std::string("Path tracing: the sky's cube: ") + error.what());
        return nullptr;
    }
}
}
