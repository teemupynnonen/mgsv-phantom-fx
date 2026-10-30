#pragma once

#include "game_lights.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// The game's shaders the add-on knows (docs/DEVELOPMENT.md, "Shaders"): its pixel shaders by their
// checksum, each with the game pass it draws, and its vertex shaders by their source name. The game
// creates every shader as it loads; a draw looks its pixel shader up once (DrawnShader), and every
// module that looks at the draw reads the same row.
namespace phantom_fx::game_shaders
{
using game_lights::Falloff, game_lights::GameLight, game_lights::Shape, game_lights::Twin;

enum class GamePass : std::uint8_t
{
    FogPass,         // Fills the fog texture at the start of the frame
    SkyLight,        // The sky's light at half size; binds the G-buffer normals
    GameOcclusion,   // The game's SSAO, whose result only the resolve reads
    IndirectUpscale, // Writes the indirect light into the diffuse light buffer
    CameraVelocity,  // Adds the camera's motion to the velocity; reads the moving meshes' at t3
    SkyProbe,        // Lights the probe atlas with the sky's light
    Sun,             // The sun's light; binds the G-buffer's specular target, which holds the material
    SkySpecular,     // Indoors, where no sun shines, the sky's specular alone in the sun pass's place
    LocalLight,      // A point or spot light: a draw that marks the stencil, then one that lights
    Resolve,         // The G-buffer and the light into the scene's colour, times the game's occlusion
    Sky,
    SkyDome,         // The sky's painted dome and its clouds, drawn over it
    Sea,             // The game's sea, drawn over the lit scene after the sky (Primitive_TppFourierOcean*)
    ReflectionMask,  // The screen-space reflections' mask from the depth, normals and roughness; reads the scene's depth at t0
    ReflectionAdd,   // The screen-space reflections blended over the scene
    Reduce,          // The scene's quarter-size copy that bloom and the lens flares start from
    Copy,            // A plain copy the game makes anywhere
    DofComposite,    // The depth of field over its copy of the scene
    MotionBlurBake,  // The velocity and depth the motion blur's gathers read, baked at half size
    MotionBlur,      // The motion blur's gather, twice at half size; a copy then blends it over the scene
    LutBuild,        // The grading's colour LUT
    Grade,           // The grading: bloom and the colour LUT
    Fxaa,
    GammaControl,    // The scene into the back buffer
};

struct GameShader
{
    GamePass pass;
    const char* name;     // Its source file's
    const char* checksum; // toHex's form of its DXBC checksum
    std::optional<GameLight> light; // LocalLight
    // Sun: inside the shadow range, with the shadow map; and with the sky's reflection, which in rain
    // the game's rain filter draws instead.
    bool isShadowMapped{}, reflectsSky{};
    bool hasTppTonemap{}; // Resolve: TppTonemap's curve, which HDR's grading takes; the first to read the fog
    // Resolve: in rain, the sky's reflection on wet surfaces added to the scene, scaled by inSpecular's (t3) y
    // alone, the specular intensity.
    bool hasRainReflection{};
    bool hasSprite{};     // Sky: the sun's or the moon's sprite
    bool isFarSea{};      // Sea: the far sea, a plane 5 m below the near sea (its material constants' c0.z)
};

inline constexpr GameShader GameShaders[]{
    {GamePass::FogPass, "VolFog_TppVolFog", "b25013a77ea177c7eab8110269b754f2"},
    {GamePass::SkyLight, "SSLighting2_SH_SkyLight_Encode", "ac49de83da6ebb80336de04c4ef7d022"},
    {GamePass::GameOcclusion, "SSAOLineIntegrals", "1e716301e9c644c4d59c620e9b84f7de"},
    {GamePass::GameOcclusion, "AmbientObscuranceSSAO", "9f963f50539aafc2ee77b888c56d3a0f"},
    {GamePass::IndirectUpscale, "BilateralUpscale2x2", "1314ac1eb31bb06b4e972fdc2c24ccbf"},
    {GamePass::CameraVelocity, "MotionBlurCameraVelocity", "5d0241dd9804f41c780cd36669c71198"},
    {GamePass::SkyProbe, "SH_SphereMap", "4a3faf87b9ec54a401060a5b736eae5a"},

    {.pass = GamePass::Sun, .name = "SunLight_DL_CS_ES", .checksum = "56ed4db892a7940f038146eeb0df0d30", .isShadowMapped = true, .reflectsSky = true},
    {.pass = GamePass::Sun, .name = "SunLight_DL_ES", .checksum = "92726e8d231c8ed85f0db90400ef2258", .reflectsSky = true},
    {.pass = GamePass::Sun, .name = "SunLight_DL_CS", .checksum = "7e5c2fc99c9d84ddd32df4e789e7082a", .isShadowMapped = true},
    {.pass = GamePass::Sun, .name = "SunLight_DL", .checksum = "853f56a63e5bc66e6ee301c85ebfeaa8"},
    {GamePass::SkySpecular, "SunLight_ES", "367e8f6128befd48f2840ab3d6ed19b4"},

    // A light's shape, falloff and twin, whether it has specular and a shadow map, and whether it is a flashlight.
    {GamePass::LocalLight, "Lighting_Point_NA", "e61cc726c5c877555261f03dbc4aa967", GameLight{Shape::Point, Falloff::Sixteenth, Twin::PointNA, true, true}},
    {GamePass::LocalLight, "Lighting_Point_NA_NShdw", "32bdd328651cf4f11ebcc47c0b65dfde", GameLight{Shape::Point, Falloff::Sixteenth, Twin::PointNA, true, false}},
    {GamePass::LocalLight, "Lighting_Point_NA_NoSpec", "f037f71a002e42f0cb07ad2768f7b8ac", GameLight{Shape::Point, Falloff::Sixteenth, Twin::PointNA, false, true}},
    {GamePass::LocalLight, "Lighting_Point_NA_NoSpec_NShdw", "259fff504d9caef06a467ff6ce9ee1bf", GameLight{Shape::Point, Falloff::Sixteenth, Twin::PointNA, false, false}},
    {GamePass::LocalLight, "Lighting_Spot_NA", "57b7365fcfb89c3f91fffcebf1a6b1e5", GameLight{Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, true, true}},
    {GamePass::LocalLight, "Lighting_Spot_NA_NShdw", "dddab560572d7352181cd83f00f6dd63", GameLight{Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, true, false}},
    {GamePass::LocalLight, "Lighting_Spot_NA_NoSpec", "7d2ed256fcff999707444bb115321c7f", GameLight{Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, false, true}},
    {GamePass::LocalLight, "Lighting_Spot_NA_NoSpec_NShdw", "91bc5dd518df1368e60d53c5f065e9f4", GameLight{Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, false, false}},
    {GamePass::LocalLight, "Lighting_Spot_NA_Mask", "d53793289e8c2824800876d8525aab2c", GameLight{Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, true, true}},
    {GamePass::LocalLight, "Lighting_Spot_NA_Mask_NShdw", "dcc81c5f2c97e20003a27a9e7db54376", GameLight{Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, true, false}},
    {GamePass::LocalLight, "Lighting_Spot_NA_Mask_NSpec", "a435549d3334a5715c165a68083ea66e", GameLight{Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, false, true}},
    {GamePass::LocalLight, "Lighting_Spot_NA_Mask_NSpec_NShdw", "fd570742642f09e699d193ffb02bf0df", GameLight{Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, false, false, true}},
    {GamePass::LocalLight, "SSLighting2_Point", "afbc12167037e4e0d653da184c7e3e82", GameLight{Shape::Point, Falloff::Quartic, Twin::Point2, true, true}},
    {GamePass::LocalLight, "SSLighting2_Point_NoShadow", "4eb687f92330ee7b11ccf8e32950e4a9", GameLight{Shape::Point, Falloff::Quartic, Twin::Point2, true, false}},
    {GamePass::LocalLight, "SSLighting2_Point_NoSpec", "a1c56a0978c706222ebeacbf6fc9ebe4", GameLight{Shape::Point, Falloff::Quartic, Twin::Point2, false, true}},
    {GamePass::LocalLight, "SSLighting2_Point_NoSpec_NoShadow", "e9cd14a10e3f1df895cb6799c232f99e", GameLight{Shape::Point, Falloff::Quartic, Twin::Point2, false, false}},
    {GamePass::LocalLight, "SSLighting2_Spot", "ac0947110cb0f17f4ffe930a6a11617d", GameLight{Shape::Spot, Falloff::Quartic, Twin::Spot2, true, true}},
    {GamePass::LocalLight, "SSLighting2_Spot_NoShadow", "74063ef2a991625184fdf283ba58c43f", GameLight{Shape::Spot, Falloff::Quartic, Twin::Spot2, true, false}},
    {GamePass::LocalLight, "SSLighting2_Spot_NoSpec", "cdc559ec2ca4d52a54697cf5ca023667", GameLight{Shape::Spot, Falloff::Quartic, Twin::Spot2, false, true}},
    {GamePass::LocalLight, "SSLighting2_Spot_NoSpec_NoShadow", "a823ffb58661c508e6270227d4910c91", GameLight{Shape::Spot, Falloff::Quartic, Twin::Spot2, false, false}},
    {GamePass::LocalLight, "SSLighting2_Spot_Mask", "9ead0f83217db6af698f860fd3233441", GameLight{Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, true, true}},
    {GamePass::LocalLight, "SSLighting2_Spot_Mask_NShdw", "2d985165a56e0622d42f086a28c305d0", GameLight{Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, true, false}},
    {GamePass::LocalLight, "SSLighting2_Spot_Mask_NSpec", "fa6ad11cfc35a2281a22b92c1a2aa6db", GameLight{Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, false, true}},
    {GamePass::LocalLight, "SSLighting2_Spot_Mask_NSpec_NShdw", "74872741b699fdefa13ae71349f41ec9", GameLight{Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, false, false}},

    {.pass = GamePass::Resolve, .name = "DR_VolFog_TppTonemap", .checksum = "00a609370c2e8b47a645263f91f6d3e8", .hasTppTonemap = true},
    {.pass = GamePass::Resolve, .name = "DR_VolFog_TppTonemap_MD", .checksum = "79c001e295254d8a749e41d1d7e49de7", .hasTppTonemap = true},
    {.pass = GamePass::Resolve, .name = "DR_TonemapRainFilter", .checksum = "9a4abc815afc5f121572d695f07af5af", .hasTppTonemap = true, .hasRainReflection = true},
    {.pass = GamePass::Resolve, .name = "DR_TonemapRainFilter_NoIR", .checksum = "47f52852eaccaa83c25745ce790a212c", .hasTppTonemap = true,
        .hasRainReflection = true},
    {GamePass::ReflectionMask, "LocalReflectionMask", "2d9cb6c5e467cff5e9c6401b4aafebb7"},
    {GamePass::ReflectionAdd, "LocalReflectionAddBuffer", "97a9ad7e0faca7b1594a38c331826a94"},
    {GamePass::Resolve, "DeferredRendering", "df87979884e90877ee0337c13e01664f"},
    {GamePass::Resolve, "DeferredRendering_VolFog", "322890810465ee05f4895bc69e80a187"},
    {GamePass::Resolve, "DeferredRenderingTone", "47256d38f45b59b6c46cf32bf29564f0"},
    {GamePass::Resolve, "DeferredRenderingTone_GVFog", "86a042757928c965fff1c016647c4dfa"},
    {GamePass::Resolve, "DeferredRenderingFilmic", "586e33bca62435478f23154da9e25187"},
    {GamePass::Resolve, "DeferredRenderingFilmic_VolFog", "504b446c3e795d75e6adcd6753c11d12"},

    {.pass = GamePass::Sky, .name = "Sky_Draw_TppBaked", .checksum = "0fddc9d03f8bd436af2eabe2bc620c44", .hasSprite = true},
    {.pass = GamePass::Sky, .name = "Sky_Draw_Tpp", .checksum = "175290aae288daa07d2440ce316c7a50", .hasSprite = true},
    {GamePass::Sky, "Sky_Draw_TppNoSky", "3cf390b8d8af0de27c093019a693b636"}, // In fog and sandstorms: the fog's colour alone
    {GamePass::SkyDome, "tpp3dfw_constant_sky", "1d8d495cfad8edf0a94795d1b83ab365"},
    {GamePass::SkyDome, "tpp3dfw_generativeClouds", "4e081ddecae4b78e7229ae6dec61d2ee"},

    // Each as the game makes it and as the float scene patches it (HDR's withoutTonemap).
    {GamePass::Sea, "Primitive_TppFourierOceanHQ", "0b2897387196afcf41609619bde5527a"},
    {GamePass::Sea, "Primitive_TppFourierOceanHQ", "799e8c706b42d3aa642843f8e73fdaf9"},
    {GamePass::Sea, "Primitive_TppFourierOceanLQ", "10ece44e892dd5f659f6cd56fed6f626"},
    {GamePass::Sea, "Primitive_TppFourierOceanLQ", "536ed28c18bb66d2828795bdcf5fc3d5"},
    {.pass = GamePass::Sea, .name = "Primitive_TppFourierOceanFill", .checksum = "51218f2d1ccc9d0f9cf159653ed41f5d", .isFarSea = true},
    {.pass = GamePass::Sea, .name = "Primitive_TppFourierOceanFill", .checksum = "9291bd0ac69ab6cdeae6244f778c4ddb", .isFarSea = true},

    {GamePass::Reduce, "CopyReductionX4", "e5b80b0a4d737e50533f9a058ffdca42"},
    {GamePass::Copy, "CopyRenderBuffer", "9f93a957f1e21656e0d86bf112b71d6d"},
    {GamePass::DofComposite, "DOF_ScatterCompositeFinal", "d1462a415f8e79dba14db617c576407d"},
    {GamePass::MotionBlurBake, "MotionBlurBakeVelocityDepth", "d89d8bcef5a286dee3f697d3aae068fa"},
    {GamePass::MotionBlur, "MotionBlurMcGuire", "475283f0d10c84d3c1b47e2ba153c916"},
    {GamePass::LutBuild, "ColorCorrection_BlndLut_Tex_Gen", "82a954d0fe3755bb0a79aaaded5cce74"},
    {GamePass::Grade, "Tonemap", "422ba739df267d9f92d4d62b0d2b4e09"},
    {GamePass::Fxaa, "Fxaa", "deef1a5e3462ab091b5612a37662d15d"},
    {GamePass::GammaControl, "GammaControl", "12790f061c5f1460aa40c264e4bc9f07"},
};

static_assert(
    [] {
        for (const auto& row : GameShaders)
            for (const auto& other : GameShaders)
                if (&row != &other && std::string_view(row.checksum) == other.checksum) return false;

        return true;
    }(),
    "every checksum names one shader");

static_assert(
    [] {
        std::array<int, static_cast<std::size_t>(Twin::Count)> twins{};
        for (const auto& row : GameShaders)
            if (row.light && game_lights::isTwin(*row.light)) ++twins[static_cast<std::size_t>(row.light->twin)];

        return std::ranges::all_of(twins, [](int count) { return count == 1; });
    }(),
    "every kind of light needs exactly one twin");

// The modules draw these passes from a single shader.
static_assert(
    [] {
        for (const auto pass : {GamePass::Fxaa, GamePass::Grade, GamePass::GammaControl, GamePass::Copy, GamePass::LutBuild, GamePass::Reduce,
                 GamePass::CameraVelocity, GamePass::SkyProbe})
            if (std::ranges::count(GameShaders, pass, &GameShader::pass) != 1) return false;

        return true;
    }(),
    "a pass drawn from a single shader has one row");

// What a vertex shader draws, from its source name: the 2D pass (Draw2D*), the game's models (their
// G-buffer and shadow passes), its terrain, or something else.
enum class VertexShader : std::uint8_t { Other, Draw2D, Model, Terrain };

// ---- On the game's threads ----
// As the game creates a pixel shader; `code` is the game's own (HDR's patched shaders are known by the
// code they were patched from). Every shader the game creates from a row's code is known.
void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Other for a shader the add-on does not know.
VertexShader vertexShaderOf(IUnknown* shader) noexcept;
// The first shader the game created from the row's code, to draw with the game's own shader (the
// ray-traced lights' twins); null until then. Held until forget.
ID3D11PixelShader* firstCreated(const GameShader& row) noexcept;
// Lets every shader go; only when no Direct3D work can run (device teardown, unload).
void forget() noexcept;

// The pixel shader a draw is drawn with: looked up on first use, then kept for the draw, so the
// modules that look at one draw look it up once between them. The modules that draw it with their
// own shaders bind the game's again after.
class DrawnShader
{
public:
    explicit DrawnShader(ID3D11DeviceContext* context) noexcept : context(context) {}
    DrawnShader(const DrawnShader&) = delete;
    DrawnShader& operator=(const DrawnShader&) = delete;

    // Null for a shader the add-on does not know, or none.
    const GameShader* row() noexcept;
    ID3D11PixelShader* object() noexcept;

private:
    void lookUp() noexcept;

    ID3D11DeviceContext* context;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
    const GameShader* found{};
    bool isLookedUp{};
};
}
