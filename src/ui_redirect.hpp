#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

// While the engine renders below the output size, its grading and 2D pass (HUD, menus, subtitles)
// would draw into the small scene target and be stretched with it. The add-on keeps an output-size
// copy of the scene target instead: the upscaler fills it each frame before the grading, the
// grading's full-size draws and the 2D pass's draws and clears are made on it at the output size,
// and GammaControl reads it in place of the scene target. docs/DEVELOPMENT.md, "The UI at the
// output size".
namespace phantom_fx::ui_redirect
{
struct DrawCall
{
    unsigned count{}, instances{}, first{}, firstInstance{};
    int vertexOffset{};
    bool indexed{};
};

// What the upscaler fills before the grading.
struct Output
{
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view; // The target, read as it is drawn
    // The engine's scene target the 2D pass meant, at the render size.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> scene;
    unsigned width{}, height{};
};

// The output-size surfaces the game's post processing draws with. The motion blur's are made apart
// from the others, so the blur alone (float scene, native resolution) does not make the full-size ones.
enum class Surface : std::uint8_t
{
    Scene,    // The output target, in place of the scene target
    Copy,     // In place of the scene target's copy that the grading reads
    Blur,     // Half the output size: in place of the motion blur's two half-size targets
    BlurTwin,
    BlurBake, // Half the output size: the motion blur's velocity and depth, in the game's 8-bit format
};

// One draw of the game's at the output size, on a recording context: `target` in place of its render
// target (without a depth-stencil), `inputs` in place of its shader resources and its viewports and
// scissors scaled to the target. Into Scene and Copy, cPSSystem (b0) has the output size; the motion
// blur keeps the game's, whose sizes set how far it reaches. Destroying it puts the game's bindings
// back. False when there are no output-size surfaces; the draw then stays as the game made it.
class OutputSizeDraw
{
public:
    struct Input
    {
        UINT slot;
        Surface surface;
    };

    OutputSizeDraw(ID3D11DeviceContext* context, Surface target, std::initializer_list<Input> inputs) noexcept;
    ~OutputSizeDraw();
    OutputSizeDraw(const OutputSizeDraw&) = delete;
    OutputSizeDraw& operator=(const OutputSizeDraw&) = delete;

    explicit operator bool() const noexcept { return context != nullptr; }

private:
    static constexpr std::size_t MaxInputs = 2;

    ID3D11DeviceContext* context{};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount{}, rectCount{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> system;
    UINT inputSlots[MaxInputs]{};
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> inputViews[MaxInputs];
    std::size_t inputCount{};
};

// Copies one output-size surface into the other on a recording context.
void copySurface(ID3D11DeviceContext* context, Surface to, Surface from) noexcept;
struct SurfaceViews
{
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    unsigned width{}, height{};
};
// The surface's target and view; empty when its surfaces could not be made.
SurfaceViews surfaceViews(Surface surface) noexcept;
// The engine renders below the output size.
bool isUpscaling() noexcept;
// The view's texture has the render size, while the engine renders below the output size.
bool renderSized(ID3D11View* view) noexcept;

void attach(ID3D11Device* device) noexcept;
void detach() noexcept;
// The swap chain's size.
void setOutputSize(unsigned width, unsigned height) noexcept;

// The 2D pass is recognised by its vertex shaders (game_shaders::VertexShader::Draw2D), bound on the
// context that records it.
void vertexShaderBound(ID3D11DeviceContext* context, bool isDraw2D) noexcept;
// FinishCommandList clears the context's state.
void contextFinished(ID3D11DeviceContext* context) noexcept;
bool drawing2D(ID3D11DeviceContext* context) noexcept;
// The context draws into the engine's scene target, where the HUD and menus go. Lens flares, the
// 2D pass's other draws, go into smaller targets.
bool drawingIntoScene(ID3D11DeviceContext* context) noexcept;

// The jitter of the frame being recorded, in render pixels (x right, y down); zero when unjittered.
using JitterSource = std::array<float, 2> (*)() noexcept;

// On a recording context. Each returns true when the call was made on the output target instead,
// so the game's own call must be skipped.
// A draw of the engine's 2D pass into the scene target. Without upscaling, a depth-tested one is made
// in place against the scene's depth without the jitter, and also returns true.
bool draw2D(ID3D11DeviceContext* context, const DrawCall& call, JitterSource jitter) noexcept;
// A clear of the scene target within the 2D pass.
bool clearTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const float color[4]) noexcept;
// GammaControl, which reads the scene target and writes the back buffer.
bool drawGamma(ID3D11DeviceContext* context, const DrawCall& call) noexcept;

// The 2D pass masks with stencil, which it clears on the scene's depth-stencil between its draws:
// every clear of a render-size depth-stencil clears the output-size one as well.
void depthStencilCleared(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, float depth, UINT8 stencil) noexcept;

// The output target and the scene target the 2D pass last meant; empty before the first redirect.
Output output() noexcept;
}
