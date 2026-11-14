#include "ui_redirect.hpp"

#include "d3d11_helpers.hpp"
#include "features/hdr/hdr.hpp"
#include "game_constants.hpp"
#include "log.hpp"
#include "render_scale.hpp"
#include "ui_redirect_shaders.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>

namespace phantom_fx::ui_redirect
{
namespace
{
using Microsoft::WRL::ComPtr;

// The scene target is B8G8R8A8_TYPELESS with UNORM views to draw and an UNORM_SRGB view that
// GammaControl reads (docs/DEVELOPMENT.md, "Resources"). With the scene in float it is float, with
// float views holding the same encoded values (hdr.hpp).
DXGI_FORMAT drawFormat()
{
    return hdr::isSceneFloat() ? hdr::sceneFormat() : DXGI_FORMAT_B8G8R8A8_UNORM;
}

DXGI_FORMAT gammaFormat()
{
    return hdr::isSceneFloat() ? hdr::sceneFormat() : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// The output-size textures, by Surface.
struct Texture
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
    unsigned width{}, height{};
};

bool isBlurSurface(Surface surface)
{
    return surface == Surface::Blur || surface == Surface::BlurTwin || surface == Surface::BlurBake;
}

struct Targets
{
    unsigned width{}, height{};
    Texture surfaces[2]; // Scene, Copy
    ComPtr<ID3D11ShaderResourceView> srgbView;
    // The 2D pass masks with stencil. Its few depth-tested draws get the scene's depth stretched in
    // first (fillDepth).
    ComPtr<ID3D11DepthStencilView> depth;
    // cPSSystem (slot b0) for the output size. The 2D pass's pixel shaders read only m_renderInfo (c1)
    // and m_renderBuffer (c2) from it, to place screen-space effects.
    ComPtr<ID3D11Buffer> systemConstants;
    // cPSSystem for a grading draw: the game's, copied before each, with c1 and c2 from
    // `systemConstants`. The grading samples the scene by pixel position over m_renderBuffer.zw.
    ComPtr<ID3D11Buffer> postSystemConstants;
    // cPSScene (slot b2): the game's, copied before each draw, with m_viewportSize.xy (c22) set to
    // the output size from `viewportSize`. The camera filters measure the screen with it, and the
    // sandstorm's vignette (Draw2D_TppNoiseFilter) is placed by pixel position over it.
    ComPtr<ID3D11Buffer> sceneConstants, viewportSize;

    const Texture& operator[](Surface surface) const { return surfaces[static_cast<std::size_t>(surface)]; }
};

// The motion blur's surfaces, made apart from the set above: in the float scene at native resolution the blur
// goes through these alone, and the set's full-size surfaces would sit unused.
struct BlurTargets
{
    unsigned width{}, height{}; // The output size they were made for
    Texture surfaces[3];        // Blur, BlurTwin, BlurBake

    const Texture& operator[](Surface surface) const
    {
        return surfaces[static_cast<std::size_t>(surface) - static_cast<std::size_t>(Surface::Blur)];
    }
};
static_assert(static_cast<int>(Surface::BlurTwin) == static_cast<int>(Surface::Blur) + 1
        && static_cast<int>(Surface::BlurBake) == static_cast<int>(Surface::Blur) + 2,
    "BlurTargets counts the blur's surfaces from Blur, in Surface's order.");

// What fills a depth-stencil with the scene's depth without the jitter (fillDepth).
struct DepthFill
{
    ComPtr<ID3D11VertexShader> wholeTarget;
    ComPtr<ID3D11PixelShader> stretchDepth;
    ComPtr<ID3D11DepthStencilState> writeDepth;
    ComPtr<ID3D11Buffer> constants; // The jitter, rewritten before each fill
    // The composite behind the scene (compositeBehind).
    ComPtr<ID3D11PixelShader> compositeBehind;
    ComPtr<ID3D11BlendState> premultiplied;
    ComPtr<ID3D11DepthStencilState> noDepth;
    ComPtr<ID3D11Buffer> compositeConstants;
};

// compositeBehind's surfaces at the target's size: the draw alone in a float layer, and the target
// before it, in the target's format.
struct CompositeSurfaces
{
    unsigned width{}, height{};
    DXGI_FORMAT format{};
    Texture layer;
    ComPtr<ID3D11Texture2D> backdrop;
    ComPtr<ID3D11ShaderResourceView> backdropView;
};

// Without upscaling, the depth a depth-tested 2D draw tests in place of the game's.
struct InPlaceDepth
{
    unsigned width{}, height{};
    ComPtr<ID3D11DepthStencilView> view;
};

// Whether each recording context has a 2D pass vertex shader bound, by the context ReShade reports
// with each binding.
struct BoundShader
{
    std::atomic<ID3D11DeviceContext*> context{};
    std::atomic<bool> draw2D{};
};

std::atomic<unsigned> outputWidth{}, outputHeight{};

// The game has 24 deferred contexts and the immediate one.
std::array<BoundShader, 64> boundShaders;
std::atomic<bool> loggedDraw{};

std::mutex mutex;
ComPtr<ID3D11Device> device;
std::shared_ptr<const Targets> targets;
std::shared_ptr<const BlurTargets> blurTargets;
// The render-size scene target the 2D pass last meant, and a view to stretch it from.
ComPtr<ID3D11Resource> scene;
ComPtr<ID3D11ShaderResourceView> sceneView;
// The render-size depth-stencil a depth-tested 2D draw last had, and a view to read its depth.
ComPtr<ID3D11Resource> sceneDepth;
ComPtr<ID3D11ShaderResourceView> sceneDepthView;
std::shared_ptr<const DepthFill> depthFill;
std::shared_ptr<const InPlaceDepth> inPlaceDepth;
std::shared_ptr<const CompositeSurfaces> compositeSurfaces;
bool creationFailed{}, blurCreationFailed{}, depthFillFailed{};

bool belowOutput(render_scale::Size render)
{
    return render.width && render.width < outputWidth && render.height < outputHeight;
}

// The engine renders below the output size, and `texture` has its render size.
bool renderSized(const D3D11_TEXTURE2D_DESC& texture)
{
    const auto render = render_scale::current();
    return texture.Width == render.width && texture.Height == render.height && belowOutput(render);
}

bool sceneTarget(const D3D11_TEXTURE2D_DESC& texture)
{
    return texture.Format == hdr::sceneFormat() && renderSized(texture);
}

bool makeSurface(Texture& surface, const D3D11_TEXTURE2D_DESC& desc, DXGI_FORMAT viewFormat)
{
    const D3D11_RENDER_TARGET_VIEW_DESC targetDesc{viewFormat, D3D11_RTV_DIMENSION_TEXTURE2D};
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{viewFormat, D3D11_SRV_DIMENSION_TEXTURE2D};
    viewDesc.Texture2D.MipLevels = 1;
    surface.width = desc.Width;
    surface.height = desc.Height;

    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, surface.texture.GetAddressOf()))
        && SUCCEEDED(device->CreateRenderTargetView(surface.texture.Get(), &targetDesc, surface.target.GetAddressOf()))
        && SUCCEEDED(device->CreateShaderResourceView(surface.texture.Get(), &viewDesc, surface.view.GetAddressOf()));
}

// A surface in the scene's format.
D3D11_TEXTURE2D_DESC surfaceDesc(unsigned width, unsigned height)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = hdr::sceneFormat();
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    return desc;
}

std::shared_ptr<const Targets> createTargets(unsigned width, unsigned height)
{
    auto created = std::make_shared<Targets>();
    created->width = width;
    created->height = height;

    auto desc = surfaceDesc(width, height);
    auto& [outputScene, outputCopy] = created->surfaces;
    if (!makeSurface(outputScene, desc, drawFormat()) || !makeSurface(outputCopy, desc, drawFormat())) return nullptr;

    const float system[16]{0, 0, 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 0, static_cast<float>(width),
        static_cast<float>(height), 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)};
    const D3D11_BUFFER_DESC systemDesc{sizeof(system), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
    const D3D11_SUBRESOURCE_DATA systemData{system};
    const D3D11_BUFFER_DESC postSystemDesc{sizeof(system), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    if (FAILED(device->CreateBuffer(&systemDesc, &systemData, created->systemConstants.GetAddressOf()))
        || FAILED(device->CreateBuffer(&postSystemDesc, nullptr, created->postSystemConstants.GetAddressOf())))
        return nullptr;

    const D3D11_BUFFER_DESC sceneDesc{sizeof(game_constants::SceneConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    const float viewport[4]{static_cast<float>(width), static_cast<float>(height)};
    const D3D11_BUFFER_DESC viewportDesc{sizeof(viewport), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
    const D3D11_SUBRESOURCE_DATA viewportData{viewport};
    if (FAILED(device->CreateBuffer(&sceneDesc, nullptr, created->sceneConstants.GetAddressOf()))
        || FAILED(device->CreateBuffer(&viewportDesc, &viewportData, created->viewportSize.GetAddressOf())))
        return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srgbDesc{gammaFormat(), D3D11_SRV_DIMENSION_TEXTURE2D};
    srgbDesc.Texture2D.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    if (FAILED(device->CreateShaderResourceView(outputScene.texture.Get(), &srgbDesc, created->srgbView.GetAddressOf()))
        || FAILED(device->CreateTexture2D(&desc, nullptr, depth.GetAddressOf()))
        || FAILED(device->CreateDepthStencilView(depth.Get(), nullptr, created->depth.GetAddressOf())))
        return nullptr;

    return created;
}

std::shared_ptr<const DepthFill> createDepthFill()
{
    auto created = std::make_shared<DepthFill>();
    const auto code = compile(shaders::WholeTarget, "ui-whole-target", "vs_5_0");
    check(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, created->wholeTarget.GetAddressOf()),
        "Creating the UI depth's vertex shader");
    created->stretchDepth = pixelShader(device.Get(), shaders::StretchDepth, "ui-stretch-depth");
    D3D11_DEPTH_STENCIL_DESC write{};
    write.DepthEnable = TRUE;
    write.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    write.DepthFunc = D3D11_COMPARISON_ALWAYS;
    check(device->CreateDepthStencilState(&write, created->writeDepth.GetAddressOf()), "Creating the UI depth's state");
    const D3D11_BUFFER_DESC constants{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    check(device->CreateBuffer(&constants, nullptr, created->constants.GetAddressOf()), "Creating the UI depth's constants");

    created->compositeBehind = pixelShader(device.Get(), shaders::CompositeBehind, "ui-composite-behind");
    D3D11_BLEND_DESC blend{};
    auto& over = blend.RenderTarget[0];
    over.BlendEnable = TRUE;
    over.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    over.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    over.BlendOp = D3D11_BLEND_OP_ADD;
    over.SrcBlendAlpha = D3D11_BLEND_ONE;
    over.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    over.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    over.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    check(device->CreateBlendState(&blend, created->premultiplied.GetAddressOf()), "Creating the UI composite's blend");
    const D3D11_DEPTH_STENCIL_DESC none{};
    check(device->CreateDepthStencilState(&none, created->noDepth.GetAddressOf()), "Creating the UI composite's depth state");
    const D3D11_BUFFER_DESC compositeConstants{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    check(device->CreateBuffer(&compositeConstants, nullptr, created->compositeConstants.GetAddressOf()), "Creating the UI composite's constants");

    return created;
}

// compositeBehind's surfaces for a target texture, made again when its size or format changes.
std::shared_ptr<const CompositeSurfaces> currentCompositeSurfaces(const D3D11_TEXTURE2D_DESC& target)
{
    std::lock_guard lock(mutex);
    if (compositeSurfaces && compositeSurfaces->width == target.Width && compositeSurfaces->height == target.Height
        && compositeSurfaces->format == target.Format)
        return compositeSurfaces;
    if (!device) return nullptr;

    auto created = std::make_shared<CompositeSurfaces>();
    created->width = target.Width;
    created->height = target.Height;
    created->format = target.Format;
    auto desc = surfaceDesc(target.Width, target.Height);
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!makeSurface(created->layer, desc, desc.Format)) return nullptr;

    desc.Format = target.Format;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{drawFormat(), D3D11_SRV_DIMENSION_TEXTURE2D};
    viewDesc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, created->backdrop.GetAddressOf()))
        || FAILED(device->CreateShaderResourceView(created->backdrop.Get(), &viewDesc, created->backdropView.GetAddressOf())))
        return nullptr;

    compositeSurfaces = std::move(created);
    log("Created the UI composite's surfaces at " + std::to_string(target.Width) + "x" + std::to_string(target.Height) + ".");

    return compositeSurfaces;
}

// The fill's shaders, made when first needed; null when they could not be.
std::shared_ptr<const DepthFill> currentDepthFill()
{
    std::lock_guard lock(mutex);
    if (depthFill || !device || depthFillFailed) return depthFill;

    try
    {
        depthFill = createDepthFill();
    }
    catch (const std::exception& error)
    {
        depthFillFailed = true;
        log(std::string("The UI's depth-tested draws keep the jittered depth: ") + error.what());
    }

    return depthFill;
}

// A depth-stencil of the scene target's size for the in-place draws (drawDepthTestedInPlace), made
// again when the size changes.
std::shared_ptr<const InPlaceDepth> currentInPlaceDepth(unsigned width, unsigned height)
{
    std::lock_guard lock(mutex);
    if (inPlaceDepth && inPlaceDepth->width == width && inPlaceDepth->height == height) return inPlaceDepth;
    if (!device) return nullptr;

    auto created = std::make_shared<InPlaceDepth>();
    created->width = width;
    created->height = height;
    auto desc = surfaceDesc(width, height);
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()))
        || FAILED(device->CreateDepthStencilView(texture.Get(), nullptr, created->view.GetAddressOf())))
        return nullptr;

    inPlaceDepth = std::move(created);

    return inPlaceDepth;
}

// The motion blur's surfaces at half the size of the scene it blends over, as the game makes its own.
// The pair it gathers between has the scene's format: the game's are 8-bit, which clips HDR highlights
// while the camera moves. The bake keeps the game's 8-bit format, so it holds what the game's would.
std::shared_ptr<const BlurTargets> createBlurTargets(unsigned width, unsigned height)
{
    auto created = std::make_shared<BlurTargets>();
    created->width = width;
    created->height = height;

    auto desc = surfaceDesc(std::max(width / 2, 1u), std::max(height / 2, 1u));
    auto& [blur, blurTwin, bake] = created->surfaces;
    if (!makeSurface(blur, desc, drawFormat()) || !makeSurface(blurTwin, desc, drawFormat())) return nullptr;

    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!makeSurface(bake, desc, desc.Format)) return nullptr;

    return created;
}

// The output-size targets, made when first needed and again when the output size changes.
std::shared_ptr<const Targets> currentTargets()
{
    const unsigned width = outputWidth, height = outputHeight;
    std::lock_guard lock(mutex);
    if (targets && targets->width == width && targets->height == height) return targets;
    if (!device || !width || !height || creationFailed) return nullptr;

    targets = createTargets(width, height);
    creationFailed = !targets;
    log(targets ? "Created the UI target at " + std::to_string(width) + "x" + std::to_string(height) + "."
                : std::string("Could not create the UI target; the UI renders at the render size."));

    return targets;
}

// The blur's surfaces, made when first needed and again when the output size changes.
std::shared_ptr<const BlurTargets> currentBlurTargets()
{
    const unsigned width = outputWidth, height = outputHeight;
    std::lock_guard lock(mutex);
    if (blurTargets && blurTargets->width == width && blurTargets->height == height) return blurTargets;
    if (!device || !width || !height || blurCreationFailed) return nullptr;

    blurTargets = createBlurTargets(width, height);
    blurCreationFailed = !blurTargets;
    log(blurTargets ? "Created the motion blur's targets at " + std::to_string(blurTargets->surfaces[0].width) + "x"
                          + std::to_string(blurTargets->surfaces[0].height) + "."
                    : std::string("Could not create the motion blur's targets; the game's blur stays its own."));

    return blurTargets;
}

// The sets one call's surfaces belong to, each made only when one of its surfaces is asked for.
struct SurfaceSets
{
    std::shared_ptr<const Targets> output;
    std::shared_ptr<const BlurTargets> blur;

    // Null when the surface's set could not be made.
    const Texture* texture(Surface surface)
    {
        if (isBlurSurface(surface))
        {
            if (!blur) blur = currentBlurTargets();

            return blur ? &(*blur)[surface] : nullptr;
        }

        if (!output) output = currentTargets();

        return output ? &(*output)[surface] : nullptr;
    }
};

void rememberScene(ID3D11View* view)
{
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    std::lock_guard lock(mutex);
    if (resource == scene) return;

    scene = resource;
    sceneView.Reset();
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{drawFormat(), D3D11_SRV_DIMENSION_TEXTURE2D};
    viewDesc.Texture2D.MipLevels = 1;
    if (device) device->CreateShaderResourceView(scene.Get(), &viewDesc, sceneView.GetAddressOf());
}

// The view format that reads a depth-stencil texture's depth; UNKNOWN when it has none.
DXGI_FORMAT depthReadFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// A view reading the depth behind `view`; null when its texture cannot be read.
ComPtr<ID3D11ShaderResourceView> depthViewOf(ID3D11DepthStencilView* view)
{
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    std::lock_guard lock(mutex);
    if (resource == sceneDepth) return sceneDepthView;

    sceneDepth = resource;
    sceneDepthView.Reset();
    const auto texture = textureDesc(view);
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{depthReadFormat(texture.Format), D3D11_SRV_DIMENSION_TEXTURE2D};
    viewDesc.Texture2D.MipLevels = 1;
    if (device && viewDesc.Format != DXGI_FORMAT_UNKNOWN && (texture.BindFlags & D3D11_BIND_SHADER_RESOURCE))
        device->CreateShaderResourceView(resource.Get(), &viewDesc, sceneDepthView.GetAddressOf());

    return sceneDepthView;
}

// The bound depth-stencil state; Direct3D's default without one.
D3D11_DEPTH_STENCIL_DESC depthState(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11DepthStencilState> state;
    UINT reference{};
    context->OMGetDepthStencilState(state.GetAddressOf(), &reference);
    D3D11_DEPTH_STENCIL_DESC desc{TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS};
    if (state) state->GetDesc(&desc);

    return desc;
}

// The bindings fillDepth changes, the game's put back when this goes out of scope.
class SavedDepthFill
{
public:
    explicit SavedDepthFill(ID3D11DeviceContext* context) : context(context)
    {
        context->IAGetInputLayout(layout.GetAddressOf());
        context->IAGetPrimitiveTopology(&topology);
        context->VSGetShader(vertex.GetAddressOf(), nullptr, nullptr);
        context->PSGetShader(pixel.GetAddressOf(), nullptr, nullptr);
        context->PSGetShaderResources(0, 1, input.GetAddressOf());
        context->PSGetConstantBuffers(0, 1, constants.GetAddressOf());
        context->RSGetState(rasterizer.GetAddressOf());
        context->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
        context->OMGetDepthStencilState(depthState.GetAddressOf(), &stencilReference);
        context->OMGetRenderTargets(TargetCount, targets, depth.GetAddressOf());
    }

    ~SavedDepthFill()
    {
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(topology);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, input.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->RSSetState(rasterizer.Get());
        context->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
        context->OMSetDepthStencilState(depthState.Get(), stencilReference);
        context->OMSetRenderTargets(TargetCount, targets, depth.Get());

        for (auto* target : targets)
            if (target) target->Release();
    }

    SavedDepthFill(const SavedDepthFill&) = delete;
    SavedDepthFill& operator=(const SavedDepthFill&) = delete;

private:
    static constexpr UINT TargetCount = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;

    ID3D11DeviceContext* context;

    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11ShaderResourceView> input;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    float blendFactor[4]{};
    UINT sampleMask{};
    ComPtr<ID3D11DepthStencilState> depthState;
    UINT stencilReference{};
    ID3D11RenderTargetView* targets[TargetCount]{};
    ComPtr<ID3D11DepthStencilView> depth;
};

// The scene's depth from the game's depth-stencil into `into` (width x height), without the jitter
// (shaders::StretchDepth), on a recording context; the stencil stays. Only the 2D pass's depth-tested
// draws need it: the sortie prep's emblem stands behind the characters by it (frame 20182, #874).
// Tested against the jittered depth, its edge along them flickered. False when it cannot.
bool fillDepth(ID3D11DeviceContext* context, ID3D11DepthStencilView* into, unsigned width, unsigned height, ID3D11DepthStencilView* gameDepth,
    std::array<float, 2> jitter)
{
    const auto fill = currentDepthFill();
    const auto view = depthViewOf(gameDepth);
    if (!fill || !view) return false;

    // The draw's test says which way is nearer; the game's depth is reversed (cleared to 0, greater_equal).
    const auto test = depthState(context).DepthFunc;
    const float nearer = test == D3D11_COMPARISON_LESS || test == D3D11_COMPARISON_LESS_EQUAL ? -1.0f : 1.0f;
    const SavedDepthFill saved(context);
    const float constants[4]{jitter[0], jitter[1], nearer};
    context->UpdateSubresource(fill->constants.Get(), 0, nullptr, constants, 0, 0);
    context->PSSetConstantBuffers(0, 1, fill->constants.GetAddressOf());
    const WholeTargetViewport viewport(context, width, height);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(fill->wholeTarget.Get(), nullptr, 0);
    context->PSSetShader(fill->stretchDepth.Get(), nullptr, 0);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(fill->writeDepth.Get(), 0);
    context->OMSetRenderTargets(0, nullptr, into);
    context->PSSetShaderResources(0, 1, view.GetAddressOf());
    context->Draw(3, 0);

    return true;
}

void issue(ID3D11DeviceContext* context, const DrawCall& call)
{
    if (call.indexed) context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    else context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
}

// The bindings compositeBehind changes, the game's put back when this goes out of scope.
class SavedComposite
{
public:
    explicit SavedComposite(ID3D11DeviceContext* context) : context(context)
    {
        context->PSGetShader(pixel.GetAddressOf(), nullptr, nullptr);
        context->PSGetShaderResources(0, InputCount, inputs);
        context->PSGetConstantBuffers(0, 1, constants.GetAddressOf());
        context->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
        context->OMGetDepthStencilState(depthState.GetAddressOf(), &stencilReference);
        context->OMGetRenderTargets(TargetCount, targets, depth.GetAddressOf());
    }

    ~SavedComposite()
    {
        context->PSSetShader(pixel.Get(), nullptr, 0);
        context->PSSetShaderResources(0, InputCount, inputs);
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
        context->OMSetDepthStencilState(depthState.Get(), stencilReference);
        context->OMSetRenderTargets(TargetCount, targets, depth.Get());

        for (auto* input : inputs)
            if (input) input->Release();

        for (auto* target : targets)
            if (target) target->Release();
    }

    SavedComposite(const SavedComposite&) = delete;
    SavedComposite& operator=(const SavedComposite&) = delete;

private:
    static constexpr UINT InputCount = 4, TargetCount = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;

    ID3D11DeviceContext* context;

    ComPtr<ID3D11PixelShader> pixel;
    ID3D11ShaderResourceView* inputs[InputCount]{};
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> blend;
    float blendFactor[4]{};
    UINT sampleMask{};
    ComPtr<ID3D11DepthStencilState> depthState;
    UINT stencilReference{};
    ID3D11RenderTargetView* targets[TargetCount]{};
    ComPtr<ID3D11DepthStencilView> depth;
};

// Whether the bound blend is the plain "over" the emblem draws with (source alpha, inverse source
// alpha, added), which compositeBehind reproduces.
bool blendsOver(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11BlendState> state;
    float factor[4]{};
    UINT mask{};
    context->OMGetBlendState(state.GetAddressOf(), factor, &mask);
    if (!state) return false;

    D3D11_BLEND_DESC desc{};
    state->GetDesc(&desc);
    const auto& target = desc.RenderTarget[0];

    return target.BlendEnable && target.SrcBlend == D3D11_BLEND_SRC_ALPHA && target.DestBlend == D3D11_BLEND_INV_SRC_ALPHA
        && target.BlendOp == D3D11_BLEND_OP_ADD && (target.RenderTargetWriteMask & 7) == 7;
}

// A depth-tested 2D draw (the sortie prep's emblem) over the target bound now, behind the scene by its
// depth (shaders::CompositeBehind), on a recording context with the draw's other state as it will be
// drawn. `toRender` is render pixels per target pixel. False, with nothing drawn, when it cannot.
bool compositeBehind(ID3D11DeviceContext* context, const DrawCall& call, ID3D11DepthStencilView* gameDepth, std::array<float, 2> jitter,
    std::array<float, 2> toRender)
{
    // Only the test the shader makes: nearer, or as near, passes. Another one (always, never, equal,
    // strictly nearer) would show what the draw does not, so the game's own state draws it instead.
    // And it writes no depth: the composite turns depth off, and a write would be lost.
    const auto state = depthState(context);
    if (!state.DepthEnable || state.StencilEnable || state.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO || !blendsOver(context)) return false;
    if (state.DepthFunc != D3D11_COMPARISON_GREATER_EQUAL && state.DepthFunc != D3D11_COMPARISON_LESS_EQUAL) return false;

    ComPtr<ID3D11RenderTargetView> targetView;
    context->OMGetRenderTargets(1, targetView.GetAddressOf(), nullptr);
    const auto fill = currentDepthFill();
    const auto depthView = depthViewOf(gameDepth);
    if (!targetView || !fill || !depthView) return false;

    const auto targetResource = resourceOf(targetView.Get());
    const auto surfaces = currentCompositeSurfaces(textureDesc(targetView.Get()));
    if (!surfaces) return false;

    const float nearer = state.DepthFunc == D3D11_COMPARISON_LESS || state.DepthFunc == D3D11_COMPARISON_LESS_EQUAL ? -1.0f : 1.0f;
    const float constants[8]{jitter[0], jitter[1], nearer, 0, toRender[0], toRender[1]};
    context->UpdateSubresource(fill->compositeConstants.Get(), 0, nullptr, constants, 0, 0);
    context->CopyResource(surfaces->backdrop.Get(), targetResource.Get());

    const SavedComposite saved(context);
    const float clear[4]{};
    context->ClearRenderTargetView(surfaces->layer.target.Get(), clear);
    context->OMSetRenderTargets(1, surfaces->layer.target.GetAddressOf(), nullptr);
    context->OMSetBlendState(fill->premultiplied.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(fill->noDepth.Get(), 0);
    issue(context, call);

    context->OMSetRenderTargets(1, targetView.GetAddressOf(), nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->PSSetShader(fill->compositeBehind.Get(), nullptr, 0);
    ID3D11ShaderResourceView* inputs[3]{surfaces->layer.view.Get(), surfaces->backdropView.Get(), depthView.Get()};
    context->PSSetShaderResources(0, 3, inputs);
    context->PSSetConstantBuffers(0, 1, fill->compositeConstants.GetAddressOf());
    issue(context, call);

    return true;
}

LONG scaled(LONG value, double scale)
{
    return static_cast<LONG>(std::lround(value * scale));
}

// The game's viewports and scissors, from the render size to the output size.
void scale(const D3D11_VIEWPORT* viewports, D3D11_VIEWPORT* scaledViewports, UINT count, double scaleX, double scaleY)
{
    for (UINT i = 0; i < count; ++i)
    {
        scaledViewports[i] = viewports[i];
        scaledViewports[i].TopLeftX = static_cast<float>(viewports[i].TopLeftX * scaleX);
        scaledViewports[i].TopLeftY = static_cast<float>(viewports[i].TopLeftY * scaleY);
        scaledViewports[i].Width = static_cast<float>(viewports[i].Width * scaleX);
        scaledViewports[i].Height = static_cast<float>(viewports[i].Height * scaleY);
    }
}

void scale(const D3D11_RECT* rects, D3D11_RECT* scaledRects, UINT count, double scaleX, double scaleY)
{
    for (UINT i = 0; i < count; ++i)
        scaledRects[i] = {scaled(rects[i].left, scaleX), scaled(rects[i].top, scaleY), scaled(rects[i].right, scaleX), scaled(rects[i].bottom, scaleY)};
}

// Fills the output's grading cPSSystem from the game's on `context`: the game's values, with
// m_renderInfo (c1) and m_renderBuffer (c2) at the output size.
bool copySystem(ID3D11DeviceContext* context, const Targets& output, ID3D11Buffer* gameSystem)
{
    D3D11_BUFFER_DESC desc{};
    if (gameSystem) gameSystem->GetDesc(&desc);
    using game_constants::SystemConstants;
    if (!gameSystem || desc.ByteWidth != sizeof(SystemConstants)) return false;

    constexpr UINT renderInfoOffset = offsetof(SystemConstants, renderInfo), sizesEnd = offsetof(SystemConstants, dominantLightDir);
    const D3D11_BOX sizes{renderInfoOffset, 0, 0, sizesEnd, 1, 1};
    context->CopyResource(output.postSystemConstants.Get(), gameSystem);
    context->CopySubresourceRegion(output.postSystemConstants.Get(), 0, renderInfoOffset, 0, 0, output.systemConstants.Get(), 0, &sizes);

    return true;
}

// Fills the output's cPSScene from the game's on `context`. The game's m_viewportSize.xy is the
// render size, the size of the scene target the draw was meant for.
bool copyScene(ID3D11DeviceContext* context, const Targets& output, ID3D11Buffer* gameScene)
{
    D3D11_BUFFER_DESC desc{};
    if (gameScene) gameScene->GetDesc(&desc);
    if (!gameScene || desc.ByteWidth != sizeof(game_constants::SceneConstants)) return false;

    constexpr UINT viewportSizeOffset = offsetof(game_constants::SceneConstants, width);
    const D3D11_BOX viewportSize{0, 0, 0, 2 * sizeof(float), 1, 1};
    context->CopyResource(output.sceneConstants.Get(), gameScene);
    context->CopySubresourceRegion(output.sceneConstants.Get(), 0, viewportSizeOffset, 0, 0, output.viewportSize.Get(), 0, &viewportSize);

    return true;
}

BoundShader* slotOf(ID3D11DeviceContext* context, bool add)
{
    for (auto& slot : boundShaders)
    {
        auto* owner = slot.context.load(std::memory_order_acquire);
        if (owner == context) return &slot;
        if (owner) continue;
        if (!add) return nullptr;

        ID3D11DeviceContext* empty{};
        if (slot.context.compare_exchange_strong(empty, context) || empty == context) return &slot;
    }

    return nullptr;
}
}

void vertexShaderBound(ID3D11DeviceContext* context, bool isDraw2D) noexcept
{
    if (auto* slot = slotOf(context, isDraw2D)) slot->draw2D.store(isDraw2D, std::memory_order_relaxed);
}

void contextFinished(ID3D11DeviceContext* context) noexcept
{
    if (auto* slot = slotOf(context, false)) slot->draw2D.store(false, std::memory_order_relaxed);
}

bool drawing2D(ID3D11DeviceContext* context) noexcept
{
    const auto* slot = slotOf(context, false);
    return slot && slot->draw2D.load(std::memory_order_relaxed);
}

bool drawingIntoScene(ID3D11DeviceContext* context) noexcept
{
    ComPtr<ID3D11RenderTargetView> view;
    context->OMGetRenderTargets(1, view.GetAddressOf(), nullptr);
    const auto texture = textureDesc(view.Get());
    const auto render = render_scale::current();

    return texture.Format == hdr::sceneFormat() && texture.Width == render.width && texture.Height == render.height;
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(mutex);
    device = gameDevice;
    creationFailed = blurCreationFailed = false;
}

void detach() noexcept
{
    {
        std::lock_guard lock(mutex);
        targets.reset();
        blurTargets.reset();
        sceneView.Reset();
        scene.Reset();
        sceneDepthView.Reset();
        sceneDepth.Reset();
        depthFill.reset();
        inPlaceDepth.reset();
        compositeSurfaces.reset();
        depthFillFailed = false;
        device.Reset();
    }
}

void setOutputSize(unsigned width, unsigned height) noexcept
{
    if (width == outputWidth && height == outputHeight) return;

    outputWidth = width;
    outputHeight = height;
    // A set that could not be made at the old size may fit at the new one.
    std::lock_guard lock(mutex);
    creationFailed = blurCreationFailed = false;
}

using BoundTargets = std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>;

// Without upscaling (DLAA), a depth-tested 2D draw into the scene target is made by the add-on in place,
// against the scene's depth without the jitter (fillDepth) on a depth-stencil of its own. Only for
// draws without stencil, as that depth-stencil does not have the game's.
bool drawDepthTestedInPlace(ID3D11DeviceContext* context, const DrawCall& call, const BoundTargets& bound, ID3D11DepthStencilView* depth,
    const D3D11_TEXTURE2D_DESC& target, JitterSource jitter)
{
    if (!depth || target.Format != hdr::sceneFormat()) return false;

    const auto state = depthState(context);
    const auto depthDesc = textureDesc(depth);
    if (!state.DepthEnable || state.StencilEnable || depthDesc.Width != target.Width || depthDesc.Height != target.Height
        || !viewportFillsTarget(context))
        return false;

    const auto shift = jitter();
    if (shift[0] == 0 && shift[1] == 0) return false;

    if (compositeBehind(context, call, depth, shift, {1, 1})) return true;

    const auto own = currentInPlaceDepth(target.Width, target.Height);
    if (!own || !fillDepth(context, own->view.Get(), target.Width, target.Height, depth, shift)) return false;

    const auto count = static_cast<UINT>(bound.size());
    context->OMSetRenderTargetsAndUnorderedAccessViews(count, bound.data(), own->view.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    issue(context, call);
    context->OMSetRenderTargetsAndUnorderedAccessViews(count, bound.data(), depth, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);

    return true;
}

bool draw2D(ID3D11DeviceContext* context, const DrawCall& call, JitterSource jitter) noexcept
{
    try
    {
        BoundTargets bound{};
        ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(static_cast<UINT>(bound.size()), bound.data(), depth.GetAddressOf());
        std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> views;
        for (std::size_t i = 0; i < bound.size(); ++i) views[i].Attach(bound[i]);

        const auto target = textureDesc(views[0].Get());
        if (!sceneTarget(target)) return drawDepthTestedInPlace(context, call, bound, depth.Get(), target, jitter);

        for (std::size_t i = 1; i < views.size(); ++i)
            if (views[i]) return false;

        const auto depthDesc = textureDesc(depth.Get());
        if (depth && (depthDesc.Width != target.Width || depthDesc.Height != target.Height)) return false;

        // The UI covers the scene target. Smaller viewports are the Mother Base emblem's build, which
        // draws its layers and mips into corners of it (1024x1024, 512x512, 256x256, then 128x128 down
        // to 8x8 in a row) and reads them back: drawn on the output target, the emblem came out black.
        if (!viewportFillsTarget(context)) return false;

        const auto output = currentTargets();
        if (!output) return false;

        const bool testsDepth = depth && depthState(context).DepthEnable;
        const auto shift = testsDepth ? jitter() : std::array<float, 2>{};

        // The 2D pass draws in clip space, so scaling its viewports and scissors is all it takes.
        const double scaleX = static_cast<double>(output->width) / target.Width, scaleY = static_cast<double>(output->height) / target.Height;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{}, scaledViewports[std::size(viewports)]{};
        D3D11_RECT rects[std::size(viewports)]{}, scaledRects[std::size(viewports)]{};
        UINT viewportCount = static_cast<UINT>(std::size(viewports)), rectCount = viewportCount;
        context->RSGetViewports(&viewportCount, viewports);
        context->RSGetScissorRects(&rectCount, rects);
        scale(viewports, scaledViewports, viewportCount, scaleX, scaleY);
        scale(rects, scaledRects, rectCount, scaleX, scaleY);

        ComPtr<ID3D11Buffer> system, gameScene;
        context->PSGetConstantBuffers(0, 1, system.GetAddressOf());
        context->PSGetConstantBuffers(2, 1, gameScene.GetAddressOf());
        const bool sceneCopied = copyScene(context, *output, gameScene.Get());
        context->OMSetRenderTargetsAndUnorderedAccessViews(1, (*output)[Surface::Scene].target.GetAddressOf(), depth ? output->depth.Get() : nullptr, 0,
            D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        context->RSSetViewports(viewportCount, scaledViewports);
        context->RSSetScissorRects(rectCount, scaledRects);
        context->PSSetConstantBuffers(0, 1, output->systemConstants.GetAddressOf());
        if (sceneCopied) context->PSSetConstantBuffers(2, 1, output->sceneConstants.GetAddressOf());
        const std::array toRender{static_cast<float>(1 / scaleX), static_cast<float>(1 / scaleY)};
        if (!testsDepth || !compositeBehind(context, call, depth.Get(), shift, toRender))
        {
            if (testsDepth) fillDepth(context, output->depth.Get(), output->width, output->height, depth.Get(), shift);
            issue(context, call);
        }

        context->OMSetRenderTargetsAndUnorderedAccessViews(1, bound.data(), depth.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        context->RSSetViewports(viewportCount, viewports);
        context->RSSetScissorRects(rectCount, rects);
        context->PSSetConstantBuffers(0, 1, system.GetAddressOf());
        if (sceneCopied) context->PSSetConstantBuffers(2, 1, gameScene.GetAddressOf());
        rememberScene(views[0].Get());

        if (!loggedDraw.exchange(true))
            log("The 2D pass draws at " + std::to_string(output->width) + "x" + std::to_string(output->height) + " while the engine renders at "
                + std::to_string(target.Width) + "x" + std::to_string(target.Height) + ".");

        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool clearTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const float color[4]) noexcept
{
    try
    {
        if (!drawing2D(context) || !sceneTarget(textureDesc(view))) return false;

        const auto output = currentTargets();
        if (!output) return false;

        context->ClearRenderTargetView((*output)[Surface::Scene].target.Get(), color);
        rememberScene(view);

        return true;
    }
    catch (...)
    {
        return false;
    }
}

void depthStencilCleared(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, float depth, UINT8 stencil) noexcept
{
    try
    {
        if (!renderSized(textureDesc(view))) return;

        if (const auto output = currentTargets()) context->ClearDepthStencilView(output->depth.Get(), flags, depth, stencil);
    }
    catch (...)
    {
    }
}

bool drawGamma(ID3D11DeviceContext* context, const DrawCall& call) noexcept
{
    try
    {
        ComPtr<ID3D11ShaderResourceView> input;
        context->PSGetShaderResources(0, 1, input.GetAddressOf());
        if (!sceneTarget(textureDesc(input.Get()))) return false;

        const auto output = currentTargets();
        if (!output) return false;

        context->PSSetShaderResources(0, 1, output->srgbView.GetAddressOf());
        issue(context, call);
        context->PSSetShaderResources(0, 1, input.GetAddressOf());
        rememberScene(input.Get());

        return true;
    }
    catch (...)
    {
        return false;
    }
}

OutputSizeDraw::OutputSizeDraw(ID3D11DeviceContext* drawing, Surface target, std::initializer_list<Input> inputs) noexcept
{
    try
    {
        SurfaceSets sets;
        const auto* surface = sets.texture(target);
        if (!surface || inputs.size() > MaxInputs) return;

        for (const auto& input : inputs)
            if (!sets.texture(input.surface)) return;

        ID3D11RenderTargetView* bound[std::size(targets)]{};
        drawing->OMGetRenderTargets(static_cast<UINT>(std::size(bound)), bound, depth.GetAddressOf());
        for (std::size_t i = 0; i < std::size(bound); ++i) targets[i].Attach(bound[i]);

        const auto gameTarget = textureDesc(targets[0].Get());
        if (!gameTarget.Width || !gameTarget.Height) return;

        viewportCount = rectCount = static_cast<UINT>(std::size(viewports));
        drawing->RSGetViewports(&viewportCount, viewports);
        drawing->RSGetScissorRects(&rectCount, rects);
        drawing->PSGetConstantBuffers(0, 1, system.GetAddressOf());
        for (const auto& input : inputs)
        {
            inputSlots[inputCount] = input.slot;
            drawing->PSGetShaderResources(input.slot, 1, inputViews[inputCount].GetAddressOf());
            ++inputCount;
        }

        const double scaleX = static_cast<double>(surface->width) / gameTarget.Width, scaleY = static_cast<double>(surface->height) / gameTarget.Height;
        D3D11_VIEWPORT scaledViewports[std::size(viewports)]{};
        D3D11_RECT scaledRects[std::size(rects)]{};
        scale(viewports, scaledViewports, viewportCount, scaleX, scaleY);
        scale(rects, scaledRects, rectCount, scaleX, scaleY);
        if (target == Surface::Scene) rememberScene(targets[0].Get());

        context = drawing;
        if (!isBlurSurface(target) && copySystem(drawing, *sets.output, system.Get()))
            drawing->PSSetConstantBuffers(0, 1, sets.output->postSystemConstants.GetAddressOf());
        for (const auto& input : inputs) drawing->PSSetShaderResources(input.slot, 1, sets.texture(input.surface)->view.GetAddressOf());
        drawing->OMSetRenderTargetsAndUnorderedAccessViews(1, surface->target.GetAddressOf(), nullptr, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        drawing->RSSetViewports(viewportCount, scaledViewports);
        drawing->RSSetScissorRects(rectCount, scaledRects);
    }
    catch (...)
    {
    }
}

OutputSizeDraw::~OutputSizeDraw()
{
    if (!context) return;

    ID3D11RenderTargetView* bound[std::size(targets)]{};
    for (std::size_t i = 0; i < std::size(targets); ++i) bound[i] = targets[i].Get();
    context->OMSetRenderTargetsAndUnorderedAccessViews(static_cast<UINT>(std::size(bound)), bound, depth.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
        nullptr, nullptr);
    context->RSSetViewports(viewportCount, viewports);
    context->RSSetScissorRects(rectCount, rects);
    context->PSSetConstantBuffers(0, 1, system.GetAddressOf());
    for (std::size_t i = 0; i < inputCount; ++i) context->PSSetShaderResources(inputSlots[i], 1, inputViews[i].GetAddressOf());
}

void copySurface(ID3D11DeviceContext* context, Surface to, Surface from) noexcept
{
    SurfaceSets sets;
    const auto *toTexture = sets.texture(to), *fromTexture = sets.texture(from);
    if (toTexture && fromTexture) context->CopyResource(toTexture->texture.Get(), fromTexture->texture.Get());
}

SurfaceViews surfaceViews(Surface surface) noexcept
{
    SurfaceSets sets;
    const auto* texture = sets.texture(surface);
    if (!texture) return {};

    return {texture->target, texture->view, texture->width, texture->height};
}

bool isUpscaling() noexcept
{
    return belowOutput(render_scale::current());
}

bool renderSized(ID3D11View* view) noexcept
{
    try
    {
        return renderSized(textureDesc(view));
    }
    catch (...)
    {
        return false;
    }
}

Output output() noexcept
{
    const auto current = currentTargets();
    std::lock_guard lock(mutex);
    if (!current) return {};

    const auto& target = (*current)[Surface::Scene];

    return {target.target, target.view, sceneView, current->width, current->height};
}
}
