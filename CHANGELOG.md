# Phantom FX changelog

## Unreleased

- **The Mother Base emblem shows again in Sortie Prep, and stands where it should.** It was a black square in front of
  Snake, the horse and the helicopter. The game builds the emblem in a corner of its screen buffer as a save loads, which
  the upscaler's full-resolution UI had taken over, so the game kept an empty emblem; and the emblem, the one UI image the
  game hides behind the characters, was tested against no depth at all. Both are as the game draws them now, with the
  upscaler's jitter taken back out of the depth, so its edge along a character no longer flickers; and where a character's
  edge covers part of a pixel, the emblem is blended in under it, so no band of backdrop shows between the two.

- **Wall lamps no longer flicker with the upscaler, however still the camera.** The game works its light volumes out on
  the CPU, already in screen space, so the upscaler's jitter reached the depth they test against but not them: a lamp
  whose volume lies on the wall passed or failed the depth test by that frame's jitter, up to 1.5 render pixels at Ultra
  Performance. The game's 24 light vertex shaders are now made from one that adds the jitter, and the ray-traced lights'
  redraw adds it too. At Africa's villa lamps, camera still, DLSS Ultra Performance, 40 frames: `0.24%` of the pixels
  flickered before, `0.000%` after.

- **A lamp drawn as two spots no longer makes its wall flicker.** Africa's villa lamps are two spots at one place, one up
  and one down. Each could win a place among the traced lights while the other lost it, so the wall was lit by the rays
  one frame and by the game's unshadowed draw the next. Lights at one place are now chosen together, and the places of
  the last frame keep a small advantage, so a place at the light cap does not change hands. At the villa's lamps with
  DLSS Ultra Performance and the camera still, the wall's brightness went `0.23 -> 0.79` in single frames; it now stays
  within `0.25 -> 0.28`.
  
- **Wall lamps no longer swap their light between frames while you move.** A lamp in Africa's villa is two spots at one
  place, one up and one down, and the game draws the downward one without a shadow map from a distance. Alike in kind,
  place and colour, the two shared one identity and one volume box, so each was cut to the other's box: the wall lit
  above the lamp one frame and below it the next. A spot's direction is now part of its identity. The lights a frame
  traces also no longer change with the upscaler's jitter, which could move a distant lamp's edges across a pixel.

- **A swaying jungle tree shades the floor under it.** The tree is skinned, so the scene took it for a character and
  dropped all its stand-ins, its canopy card among them: the sun then reached 12.5% of the frame where the game's shadow
  map blocked it, and the jungle's rock came out flat and bright. A character's stand-in with the decal alpha bit is now
  kept as a rigid model's cut-out stand-in, which only rays towards lights meet and whose alpha lets them through: at the
  case's rock the sun's shadow now disagrees on 4.3% of the frame. A character's other stand-ins, copies of an outfit, stay
  dropped.

- **The jungle floor keeps its shade under the trees.** A tree fern's fronds are drawn in the main view alone, and the
  game casts their shadow through a flat card hung in the crown. The rays saw neither, so the canopy hid nothing and the
  floor under it went flat and bright. The card is now in the scene for the sun and the lamps alone, as the game's shadow
  map has it, and it casts through its alpha: at the case's ground pixel the floor is as dark as the game's. The fronds
  stay out of the scene: in, every global illumination and reflection ray paid to test them, 4.5 of a 6.7 ms frame at the
  case, and the card already casts that shade for 2 ms.

## 2.0.1

### Fixed

- Ray-traced light where things touch the ground: no pale glow under Snake's arm when he lies prone, and no black
  patches on the ground by a boot or a knee resting on a slope.
- The ray-traced ground is now the game's own terrain as it draws it, near and far, so shadows, bounce light and
  reflections meet the ground where it really is.
- Ray-traced bounce light and reflections no longer leak in from outside: no orange glow or pale lines along pipes,
  frames and furniture standing close to a room's wall.

## 2.0.0

### New

- **Lighting Mode: Ray traced: Ultra** (path tracing, NVIDIA RTX with DLSS):
  - Traced reflections on every surface, sharp on smooth ones and spread on rough ones, at the strength the game gives
    each material. **Reflections** slider.
  - Sunlight and every lamp and flashlight come from the rays, each with its own soft shadow.
  - Reflections keep materials' roughness, shine and normal maps, and show the game's real sky.
  - Glass and mirrors reflect what is really around them. Tinted lenses stay dark.
  - Rivers and other liquids get traced reflections.
  - Glowing bulbs and signs light their surroundings. **Self-lit surfaces** slider.
- **DLSS Ray Reconstruction** for the ray-traced effects (NVIDIA RTX): replaces DLSS and the add-on's denoisers, with
  NVIDIA's presets.
- **Sea**: replaces the sea around Mother Base with ocean waves that follow the wind and weather, with the game's foam on
  breaking crests. It reflects and takes shadows like the rest of the world. Sliders for **Wavelength**, **Height**,
  **Ripples**, **Choppiness**, **Long waves**, **Wave speed** and **Foam**. About 0.3 ms.
- **Water caustics**: sunlight patterns on river beds, with an **Intensity** slider. Needs no ray tracing.
- **Glass in the ray-traced scene**: windows cast shadows, and light through them dims with the viewing angle and the
  pane's dirt.
- **Depth of field: Background blur**: **Lens** (default, a real lens's smooth blur) or **MGSV** (the game's).
- **Opacity micromaps** (Global illumination, off by default, needs a restart): faster rays through foliage on RTX 40 and
  newer. About 4 ms saved in the jungle at night.
- **Photo mode**: hold C to move the camera slowly (0.5 m/s).
- **Diagnostics**: debug views for the global illumination's denoiser (NRD) and for the ray-traced scene.

### Changed

- **New menu**, laid out like a game's graphics menu:
  - Tabs: **Graphics**, **Sandbox**, **Budget** and **Diagnostics**.
  - One line per effect with its value and cost. Click it to fold out its settings.
  - A dot marks changed settings. Each has its own reset, plus **Reset all**.
- **Lighting Mode**: Off, Screen-space, Ray traced or Ray traced: Ultra. Replaces the Budget page's presets and leaves
  the frame rate and fog alone.
- **One switch each for soft shadows and contact shadows**, shared by the sun and the lamps. Screen-space mode now gives
  lamps soft shadows too. `soft_local_shadows` and `light_screen_space_shadows` in PhantomFX.ini are no longer read.
- **SDR keeps the picture in 16-bit colour** until the end, so fog and sky no longer band. About 0.3 ms at 4K.
- **Global illumination: light keeps bouncing** until it fades, instead of stopping after three bounces. Bright, closed
  rooms gain the most.
- **Motion blur is halved while frame generation runs**, as NVIDIA recommends.

### Performance

- Ray traced: Ultra:
  - Each pixel checks only the lights that can reach it: 4.4 ms saved at Mother Base at night.
  - Foliage textures are cheaper to test: up to 2 ms saved in the jungle.
  - Rays skip moonlight too faint to show.
  - Glass costs less to trace.
- **Short rays** (Global illumination, on by default): rays look 100 m and take the sky's light past that. About 0.7 ms
  saved in Afghanistan and 0.3 ms in the jungle at 4K.
- The CPU hands ray-tracing work to the GPU sooner: about 0.5 ms saved at Mother Base at night.

### Fixed

- Ray-traced scene:
  - Characters, the helicopter and swaying trees went missing with some other mods installed.
  - Destroyed props kept casting shadows and bouncing light.
  - The helicopter's cabin had no walls, so sunlight flooded in.
  - Clothes and other layered materials had the wrong colour in reflections and bounced light (Kaz's coat was white).
- Rays started inside walls in room corners and drew bright lines along the seams. Dim rooms are now about twice as dark.
- Global illumination:
  - Light flickered along object edges and in grass with upscaling.
  - The denoiser misjudged motion near the camera.
  - Spot lights' cone edges gave bright specks.
- Ray-traced shadows:
  - A lit halo around sunlit characters in front of shaded walls.
  - Flicker along object edges with upscaling.
- Depth of field: jagged steps around in-focus edges, worse at lower render sizes.
- Motion blur:
  - The **Strength** slider did nothing.
  - A moving character's outline showed stair steps with upscaling.

## 1.9.0

This release focuses on shadows, with a lot of it built by ZioPao.

### New

- **Lamp soft shadows (PCSS)**: a lamp's shadow is sharp where it meets a surface and softens with distance. Off by
  default.
- **Contact shadow settings**: **contrast**, **reach**, **thickness** and **fade**, for the sun and the lamps.
- **Motion blur** on the Display page: the game's blur, now also with upscaling, with a **Strength** slider.

### Changed

- Contact shadows fade out with distance and leave far shadows to the shadow map.
- Foliage casts contact shadows, so clumps shade themselves.
- Lamp contact shadows cost a fraction of what they did.
- The **Shadows** page is reorganised.
- Ray-traced shadows:
  - Hair lets some light through instead of casting solid shadows.
  - Scaffold tarps cast shadows.

### Fixed

- HDR: the sun was a ring instead of a disc.
- A fine grid pattern over dark areas and fog with upscaling. In SDR, smooth fog can show slight banding (#54).
- Motion blur was pixelated with upscaling, and clipped HDR highlights.
- Lamp contact shadows stopped working past 10 m when **Contact shadow thickness** was raised.
- Ray-traced shadows:
  - Sunlight came through building walls indoors.
  - A Mother Base wall turned black up close with global illumination.

## 1.8.0

This release focuses on fixing and improving the visuals of the prologue's hospital.

- Fix the intro breaking with ray-traced lights on: the lamps were drawn into the hospital mirror's
  view too. The mirror is left to the game, and DLSS takes the frame's main camera.
- Ray-traced lights stop at the game's light volumes for the global illumination's bounce and the
  lamp shadows too. Big fill lights and a moon spot no longer tint the warm ward blue.
- Ray-traced shadows follow what the game's own shadow maps hold:
  - Light passes through a surface met from behind, so the searchlight comes in through the
    hospital windows, and low-poly rocks no longer shadow themselves where light meets shade.
  - Meshes the game never draws into its shadow maps cast no ray-traced shadow. A stairwell window
    no longer keeps the moonlight off its wall.
  - Invisible shadow-only stand-ins no longer block lamps.
  - Mesh groups the game hides cast no shadow. The hidden rubble over the hospital entrance no
    longer leaves dark blots on its porch.
- A spot light's ray-traced shadow starts where the game's shadow map starts.
- A stale sun no longer lingers after a menu, in the global illumination and the physical sky.
- Fix upscaling in exclusive fullscreen: the image stayed at the render size in a box in the
  middle of the screen, HUD included, instead of filling it.
- The menu and the Budget page name each effect the same way: global illumination shows RTGI on
  both, and the upscaler's item shows DLSS or FSR beside its name.
- Remove **Old paths** from the Diagnostics page: the ray tracing's earlier behaviour, kept to compare
  against, is gone. The ray-traced shadows, lights and global illumination are unchanged.
- **Sun size** now also sets the sun's disc for the ray-traced global illumination, which had a
  fixed 0.54 degrees, and shows under **Global illumination** too. At the default 0.53 degrees it
  looks as before.
- Remove **Draw distance**. The game loads and draws its world as it does without the add-on.

## 1.7.0

- Add **presets** on the Budget page: **Screen-space** (soft and contact shadows, GTVBAO, SSILVB)
  or **Ray-traced** (ray-traced shadows, lights and global illumination). Both unlock the frame
  rate and turn on the volumetric fog with the physical sky.
- A first start, without a settings file, uses the Screen-space preset at Quality: DLSS on NVIDIA,
  FSR on other GPUs.
- The left-hand list scrolls with the mouse wheel when the window is too short for it.
- Fix scattered rocks and stones in the ray-traced scene sitting up to 34 cm from where the game
  draws them: a rock's ray-traced shadow fell on its own top, and rocks cast shadows from empty
  ground.
- DLSS and FSR now run before most of the game's post processing, as NVIDIA recommends: the lens
  flares, bloom and the colour grading go over the clean, anti-aliased image, and when upscaling they
  draw at the output resolution. The depth of field stays before DLSS, which keeps its focus edges
  steady. With HDR, DLSS gets the scene's light before any grading.
- Remove **Depth of field** and its **Aperture** shapes. The game's own depth of field stays, and
  photo mode blurs with it: **f-stop**, F4 and the autofocus work as before.

## 1.6.0

- **Global illumination** rebuilt: rays from the pixels, each hit lit afresh every frame, with
  NVIDIA's NRD denoiser (RELAX). Light no longer lingers for a second or leaves blotches as the
  camera moves, and grass and bushes no longer boil.
- Add **Ray quality** to global illumination: Low 1, Medium 2 or High 4 rays (Medium by default).
- Fix wires, fences and grass blades flickering in global illumination with the upscaler's jitter.
- Ray-traced lights: light stays inside the game's light volumes, so it no longer leaks through
  walls and floors where no ray was stopped. Spot light beams (moonlight through a vent) stay the
  game's.
- Ray-traced lights: softer, cleaner lamp shadow edges.
- Ray-traced shadows: rooms' inner walls, floors and ceilings are now in the scene. Indoors, a
  shelf's shadow no longer hangs below it, and no dark line crosses the wall above a door.
- Ray-traced shadows: a helicopter's windows no longer keep the sun out of its cabin.
- The sun and moon move smoothly. The game moved them in steps, so shadows ticked forward about once
  a second.
- Physical sky: no grey band between the sea and the horizon at Mother Base.

## 1.5.1

- Ray-traced shadows: fix characters' own shadows. Hair cast none, the bald head under it cast a
  jagged one, and a low-detail stand-in of the outfit shadowed the neck and chest.
- Ray-traced shadows: fix stripes along the folds of Snake's suit, and black blotches flickering on
  faces in the helicopter.
- Ray-traced shadows: fix shadows of the leaves of a bush the camera backs into.
- Global illumination: leaves let light through, so bushes are no longer black inside.
- Global illumination: a flashlight's bounce lingers about a second again after the light moves.
  1.5.0's faster fade made dark speckles flicker.
- The Budget page splits the ray tracing's cost per effect: its rays, its filter and its share of
  the scene.

## 1.5.0

- Add **Ray-traced lights**: every lamp, spot light and flashlight casts shadows from rays traced
  through the game's own scene, where most of the game's lights shone through walls and characters.
  With global illumination on, their light bounces too. **Ray quality** (1 to 8 rays a pixel) and
  **Flashlight brightness** (1x to 16x, 8x by default) in the overlay. Needs DXR 1.1. Off by default.
- Add **Draw distance**: Vanilla, Extended or Far. Extended loads full detail out to about 450 m
  instead of 320 m and the distant world to about 1.8 km, keeps more detail at a distance and draws
  models and vegetation farther. Far loads the distant world to about 2.3 km. Loading farther takes
  effect when a location loads. Costs memory and CPU time.
- Ray-traced shadows: **Show the game's shadows too, near you** adds the game's shadow map within
  about 90 m.
- Ray tracing now includes characters and what they hold, posed as the game draws them, and reads
  the scene only from the game's memory.
- Global illumination lights rays that reach the sky with the game's sky light, or its baked light
  where it has none, so Mother Base is no longer dark red.
- New overlay: pages for Shadows, Lighting, Atmosphere, Camera, Display and World, and a Budget page
  with each effect's GPU cost and its switch.
- Remove the photo mode lens shot (F5) and its `lens_shot_frames` setting.
- `PhantomFX.log` now logs only start-up, sizes and failures, not periodic statistics.

## 1.4.0

- **Ray-traced shadows** and **global illumination** now trace the game's own world, read from its
  memory: every model it has loaded, with its textures, the terrain from its heightmap, and the
  plants and props it scatters over the ground. Buildings, rocks and trees out of view keep casting
  shadows and bouncing light, and see-through parts (fences, pylons, gratings, leaves) cast shadows
  with their holes.
- Fix rays going wrong everywhere when the camera got close to a wall.
- Fix black patches in cliff hollows, from hidden rocks the game keeps there.
- Fix terrain shadows flickering.
- Fix tall lattices' shadows (power pylons) breaking up at the default sun size.

## 1.3.0

- Add **Global illumination (GI-1.2)**: ray-traced indirect light after AMD's GI-1.2. Sunlight and
  skylight bounce between surfaces, also those off screen, and the sky only lights what it can
  reach. Replaces the game's baked probe light and SSAO, and GTVBAO and SSILVB while on. Needs a
  GPU that traces rays (DXR 1.1). Off by default.
- Add **Ray-traced shadows**: the sun's shadows from rays traced through the game's world, in
  place of the game's shadow map, soft shadows and sun contact shadows. They soften with distance
  as the sun's disc makes them and reach as far as you can see, where the game's end at about
  90 m. Leaves and grass keep the game's shadow map near you. Needs a GPU that traces rays
  (DXR 1.1). Off by default.
- Ray-traced shadows and global illumination move to their own overlay section, **Ray Tracing
  [EXPERIMENTAL]**.
- Add a **Character** swapper under Utility: play as Venom Snake, the avatar, a male or female
  soldier, Ocelot or Quiet, in any outfit the game has a model for. Soldiers can take any face.
- Remove **Double shadow resolution**: it cost 1.5 GB of video memory for no visible gain. The
  `double_shadow_resolution` line in `PhantomFX.ini` is now ignored.

## 1.2.0

- Add **Double shadow resolution**: the sun's shadow map at twice the width and height of the
  game's shadow quality (16384x16384 at Extra High), for sharper shadow edges and small shadows.
  Uses 768 MB more video memory. Off by default.
- Add **Haze** to the volumetric fog's physical look: from clear air to as thick as the game's own
  haze.
- Photo mode: weather and time of day changes now show while the game is paused.
- Fix the sandstorm's vignette sitting off-centre while upscaling.
- Diagnostics shows what each effect costs on the GPU: a table with a bar, milliseconds and share
  of the GPU frame for upscaling, the sun and lamp shadows, GTVBAO, SSILVB, the volumetric fog, the
  depth of field and HDR, each opening to its passes. "Show the costs over the game" keeps it in a
  corner with the overlay closed, and `PhantomFX-performance.txt` beside the log holds the same
  numbers. "Measure the real cost" times the GPU frame with everything on, then leaves out one
  effect at a time, so what the passes change in the game's own work counts too. Nothing is timed
  while the costs are not on screen.
- **Indirect lighting (SSILVB)** costs about a quarter of what it did: it now traces at half the
  render size and upscales the result along depth edges.
- **Ambient occlusion (GTVBAO)** and SSILVB read smaller copies of the depth for distant samples
  (XeGTAO's depth mips), which keeps them cheaper up close to walls and floors.
- **Lamp contact shadows** cost far less: each lamp now traces only the part of the screen it can
  light, instead of the whole screen.

## 1.1.0

- Rename the mod from MGSV DLSS to Phantom FX. Its files are now `PhantomFX.*`: delete
  `MGSVDLSS.addon64`, and rename `MGSVDLSS.ini` to `PhantomFX.ini` to keep your settings.
- Add NVIDIA DLSS Frame Generation: 2x to 6x, or Dynamic to reach the display's refresh rate. It
  runs with any DLSS mode, DLAA included, and turns on NVIDIA Reflex; the overlay shows the
  latency. The game then presents through Direct3D 12 by way of NVIDIA Streamline, so turning it
  on takes a restart. Needs an RTX 40 (2x) or RTX 50 GPU and Windows' hardware-accelerated GPU
  scheduling.
- With frame generation on, ReShade and its add-ons still see the game as Direct3D 11, so add-ons
  that follow the game's DLSS keep working.
- Add **AMD FSR** upscaling, chosen with the new **Upscaler** setting: FSR 4 on Radeon RX 7000 and
  9000, FSR 3.1 on other GPUs. It takes DLSS's place with the same modes at AMD's ratios, Native AA
  in place of DLAA, and the same sharpening.
- Add **FSR frame generation** (2x) on any GPU, with either upscaler. Like DLSS Frame Generation it
  takes a restart; it has no Reflex.
- Add **HDR10 output**: the sun, sky, lamps, fire and reflections keep their light beyond the game's
  white, up to the display's peak, while the rest looks as the game graded it. **Peak brightness**
  (the display's own by default) and **Paper white** in the overlay. Needs HDR on for the display in
  Windows; turning it on takes a restart. Off by default.
- Add **Soft shadows (PCSS)**: contact-hardening soft shadows from the sun's shadow map, sharp
  where an object meets the ground and softer further from it. **Sun size** in the overlay. Off by
  default; works in any mode and applies at once.
- Add **Sun contact shadows** and **Lamp contact shadows**: Bend Studio's screen-space shadows add
  contact shadows to the sun's shadow map (feet, rocks, grass) and give lamps, spot lights and
  flashlights short shadows of their own. The sun's are cheap; the lamps' cost one trace per light,
  so they are a separate toggle. Grass, bushes and leaves receive them but cast none, so clumps
  don't darken themselves; the shadow map still shadows them. Off by default; works in any mode and
  applies at once.
- Add **Ambient occlusion (GTVBAO)**: ground-truth visibility bitmask ambient occlusion in place of
  the game's SSAO, on the indirect light only, so it no longer darkens sunlight. Radius and strength
  in the overlay. Off (the game's own SSAO) by default; works in any mode and applies at once.
- Add **Indirect lighting (SSILVB)**: screen-space indirect lighting with visibility bitmask, one
  bounce of sunlight and lamplight between nearby surfaces, tinted by their colour. Radius and
  strength in the overlay. Off by default; works in any mode and applies at once.
- Add **Volumetric fog**: the game's own haze and fog with the sun shadowed in it, for light shafts
  through trees, buildings and dust. **Look** Physical draws the sky and the air as in nature
  (Hillaire's atmosphere, as in Unreal Engine) with height fog that follows the game's weather.
  Off by default; works in any mode and applies at once.
- Add **Depth of field**: a gather in place of the game's sprite-based depth of field, smooth at
  full resolution where the blur is slight, with blurred foreground spreading over what lies
  behind it and lights blurred before the tonemap. **Aperture** shapes out-of-focus lights with 5
  to 9 blades. Off (the game's own) by default; works in any mode and applies at once.
- The overlay is grouped into **Upscaling**, **Visuals**, **Utility** and **Diagnostics**, with
  each feature's settings indented under it.
- Sliders set away from their default get a **Reset** button.
- Add a photo mode on **F2**: the game freezes, the HUD hides, and a free camera flies with WASD,
  the mouse, Space and Ctrl (Shift for faster).
- **F3** hides the HUD, menus and subtitles at any time.
- Photo mode focuses on the centre of the screen. **f-stop** in the overlay's Utility section sets the
  blur, and **F4** turns it off and on.
- **F5** in photo mode takes a lens shot: the frozen scene rendered from points across the lens's
  opening and averaged, as a real lens forms its image. **Lens shot frames** sets how many.

## 1.0.0

Initial release.

- Replace the game's FXAA with NVIDIA DLAA, or upscale with DLSS Quality, Balanced, Performance,
  Ultra Performance or a custom render scale. The game renders its 3D scene at the lower
  resolution itself; the HUD, menus and subtitles stay at full resolution.
- Add the camera jitter and motion vectors DLSS needs, which the game has no use for itself.
- Give characters motion vectors whatever the game's motion blur setting, and leave the blur out
  while DLSS runs, so it does not smear the DLSS image.
- Keep DLSS running with the game's post processing turned down, which would otherwise turn off the
  FXAA that DLSS replaces.
- Keep texture detail in the upscaling modes with a matching texture detail bias.
- Add an **Unlock frame rate** option that lifts the game's 60 fps cap, in any mode.
- Let the game start under ReShade: on its own it closes at start-up under any Direct3D wrapper.
- Configure the mode, DLSS preset, sharpening (AMD FidelityFX RCAS) and texture detail bias in the
  ReShade overlay. Settings are saved beside the add-on, with a log for troubleshooting.
- Target the Steam release of the game, version 1.0.15.4 (title screen "Ver. 1.15"), on NVIDIA RTX
  GPUs.
