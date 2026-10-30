<p align="center">
  <img src="docs/images/header.webp" alt="Phantom FX">
</p>

<p align="center">
  AIO visual enhancement ReShade add-on for <b>Metal Gear Solid V: The Phantom Pain</b>
</p>

<p align="center">
  <a href="https://github.com/teemupynnonen/mgsv-phantom-fx/releases/latest"><img src="https://img.shields.io/github/v/release/teemupynnonen/mgsv-phantom-fx?label=release&color=d4ff00&labelColor=1f1f1f" alt="Latest release"></a>
  <a href="https://github.com/teemupynnonen/mgsv-phantom-fx/releases"><img src="https://img.shields.io/github/downloads/teemupynnonen/mgsv-phantom-fx/total?color=d4ff00&labelColor=1f1f1f" alt="Downloads"></a>
  <a href="https://www.nexusmods.com/metalgearsolidvtpp/mods/2672"><img src="https://img.shields.io/badge/Nexus%20Mods-2672-d98f40?labelColor=1f1f1f" alt="Nexus Mods"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/teemupynnonen/mgsv-phantom-fx?color=d4ff00&labelColor=1f1f1f" alt="License"></a>
</p>

<p align="center">
  <a href="https://github.com/teemupynnonen/mgsv-phantom-fx/releases/latest"><b>Download</b></a>
  &nbsp;·&nbsp;
  <a href="https://www.nexusmods.com/metalgearsolidvtpp/mods/2672"><b>Nexus Mods</b></a>
  &nbsp;·&nbsp;
  <a href="docs/PREVIEWS.md"><b>Previews</b></a>
</p>

> [!WARNING]
> Very experimental! Features are still being implemented. Expect issues with performance and visual artifacts.

> [!CAUTION]
> Playing online with this mod may get you banned.

![Gate: vanilla and Phantom FX (ray-traced), flipping](docs/images/gate-flip-4k.webp)

*Gate: morning, sunny.*

![Canyon: vanilla and Phantom FX (ray-traced), flipping](docs/images/canyon-flip-4k.webp)

*Canyon: noon, sunny.*

More comparisons: [Previews](docs/PREVIEWS.md)

## Features

### Upscaling and frame rate

- **DLSS:** DLAA and upscaling
- **DLSS Ray Reconstruction:** denoises the ray-traced lighting and upscales, with any ray-traced effect on
- **FSR:** Native AA and upscaling, FSR 4 on Radeon RX 7000 and 9000, FSR 3.1 elsewhere
- **DLSS Frame Generation:** 2x to 6x, with NVIDIA Reflex
- **FSR frame generation:** 2x, on any GPU
- **Unlock frame rate:** lifts the 60 fps cap

### Ray tracing

Needs DXR 1.1.

- **Ray-traced shadows** for the sun
- **Ray-traced lights:** shadows for every lamp, spot light and flashlight
- **Ray-traced global illumination** (NVIDIA NRD denoiser)
- **Path tracing:** traced reflections, sunlight and lamplight with real materials, glowing surfaces that light the scene and the game's own sky in every reflection, with DLSS Ray Reconstruction

### Lighting and atmosphere

- **Soft shadows** (PCSS)
- **Contact shadows** (Bend Studio's screen-space shadows)
- **Ambient occlusion** (GTVBAO)
- **Indirect lighting** (SSILVB)
- **Volumetric fog** with light shafts and a physical sky
- **Sea:** ocean waves around Mother Base, lit and reflected like the rest of the world

### Output and tools

- **HDR10 output**
- **Photo mode**
- **Weather and time of day controls**
- **Character swapper:** Snake, the avatar, soldiers, Ocelot or Quiet, in any outfit

## Requirements

- MGSV: TPP, Steam version 1.0.15.4 (title screen "Ver. 1.15")
- DLSS: NVIDIA RTX GPU. FSR: any GPU with Direct3D 12
- DLSS Frame Generation: RTX 40 (2x) or RTX 50 (up to 6x), and hardware-accelerated GPU scheduling on
- HDR: HDR on in Windows display settings
- [ReShade 6.8+ with full add-on support](https://reshade.me/#download)

## Install

1. Install ReShade for `mgsvtpp.exe` with DirectX 10/11/12. Skip the effects.
2. Extract the ZIP into the game folder.
3. In game, press <kbd>Home</kbd> and open **Phantom FX**. The screen-space effects are on, upscaled at Quality: DLSS on NVIDIA, FSR on other GPUs. With a ray-tracing GPU, try the **Ray-traced** preset on the Budget page.

> [!IMPORTANT]
> ReShade alone crashes the game at start-up. Phantom FX fixes that, so keep both installed.

## Hotkeys

| Key | Action |
| --- | --- |
| <kbd>F1</kbd> | Visual enhancements on/off |
| <kbd>F2</kbd> | Photo mode |
| <kbd>F3</kbd> | Hide HUD |
| <kbd>F4</kbd> | Photo mode blur on/off |

Photo mode: <kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd> to move, mouse to look, <kbd>Space</kbd> / <kbd>Ctrl</kbd> up/down, <kbd>Shift</kbd> faster.

## Notes

- Enabling frame generation, switching it between DLSS and FSR, or enabling HDR requires a game restart.

## Uninstall

Delete `PhantomFX.*`, the `PhantomFX` folder, `nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll`, `sl.*.dll` and `amd_fidelityfx_*.dll`. Uninstall ReShade.

## Bugs

Attach `PhantomFX.log`, `ReShade.log` and `sl.log` from the game folder.

## Development

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## License

MIT.

Third-party licenses are in the ZIP's `PhantomFX` folder.
