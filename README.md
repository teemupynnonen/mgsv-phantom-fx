# Phantom FX

AIO visual enhancement ReShade add-on for Metal Gear Solid V: The Phantom Pain.

**Very experimental! features are currently being implemented. There are bound to be issues with performance and visual artifacting.**

**Caution: Playing online with this mod may get you banned.**

![Gate: vanilla and Phantom FX (ray-traced), flipping](docs/images/gate-flip-4k.webp)

*Gate: morning, sunny.*

![Canyon: vanilla and Phantom FX (ray-traced), flipping](docs/images/canyon-flip-4k.webp)

*Canyon: noon, sunny.*

More comparisons: [Previews](docs/PREVIEWS.md)

## Features

- **DLSS:** DLAA and upscaling
- **FSR:** Native AA and upscaling, FSR 4 on Radeon RX 7000 and 9000, FSR 3.1 elsewhere
- **DLSS Frame Generation:** 2x to 6x, with NVIDIA Reflex
- **FSR frame generation:** 2x, on any GPU
- **HDR10 output**
- **Unlock frame rate:** lifts the 60 fps cap
- **Ray-traced shadows** for the sun (needs DXR 1.1)
- **Ray-traced lights:** shadows for every lamp, spot light and flashlight (needs DXR 1.1)
- **Ray-traced global illumination** (NVIDIA NRD denoiser, needs DXR 1.1)
- **Soft shadows** (PCSS)
- **Contact shadows** (Bend Studio's screen-space shadows)
- **Ambient occlusion** (GTVBAO)
- **Indirect lighting** (GT-VBGI)
- **Volumetric fog** with light shafts and a physical sky
- **Draw distance:** loads full detail and the distant world farther, with more detail at a distance
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
3. In game, press Home and open **Phantom FX**. The screen-space effects are on, upscaled at Quality: DLSS on NVIDIA, FSR on other GPUs. With a ray-tracing GPU, try the **Ray-traced** preset on the Budget page.

ReShade alone crashes the game at start-up. Phantom FX fixes that, so keep both installed.

## Hotkeys

| Key | Action |
| --- | --- |
| F1 | Visual enhancements on/off |
| F2 | Photo mode |
| F3 | Hide HUD |
| F4 | Photo mode blur on/off |

Photo mode: WASD to move, mouse to look, Space/Ctrl up/down, Shift faster.

## Notes

- Enabling frame generation, switching it between DLSS and FSR, or enabling HDR requires a game restart.
- A new draw distance loads farther or nearer once a location loads, as on Continue from the title screen.

## Uninstall

Delete `PhantomFX.*`, the `PhantomFX` folder, `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `sl.*.dll` and `amd_fidelityfx_*.dll`. Uninstall ReShade.

## Bugs

Attach `PhantomFX.log`, `ReShade.log` and `sl.log` from the game folder.

## Development

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## License

MIT. 

Third-party licenses are in the ZIP's `PhantomFX` folder.
