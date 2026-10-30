# Phantom FX changelog

## Unreleased

- **The sea.** A new **Sea** switch under atmosphere replaces the sea around Mother Base: waves from a real
  ocean spectrum that rise and roll with the wind, bigger in rain, with foam where their crests break. It is
  lit like the rest of the world, so it reflects the sky, the sun and the platforms and takes their shadows;
  with path tracing its reflections are traced. The ray-traced light under the platforms now bounces off the
  water instead of the sky. A **Waves** slider sets the wind against the weather's. It costs about 0.3 ms.
- **Path tracing.** A new **Path tracing** switch under lighting, with DLSS Ray Reconstruction and the
  global illumination on: reflections are traced on every surface, sharp on smooth ones and spread on
  rough ones, in place of the game's screen-space and sky reflections (water and glass keep the game's;
  in rain, wet surfaces take the traced ones), and the sun and every lamp and flashlight light the scene
  from the path tracer, each light with its own soft shadow where lights overlap. Each surface reflects
  with the strength the game gives its material, and a **Reflections** slider makes them stronger or
  weaker. Rays now see what the camera sees: surfaces they hit keep their roughness, shine and normal maps,
  so shiny things stay shiny in reflections and reflections show other reflections. Glowing bulbs and signs
  light what is around them, with a **Self-lit surfaces** slider. Reflections of the sky show the game's real
  sky, with its horizon, clouds, sun glow and weather.
- **DLSS Ray Reconstruction.** A new **Ray Reconstruction** switch under the upscaler, for NVIDIA RTX GPUs,
  shown while a ray-traced effect is on: NVIDIA's model cleans up the ray-traced global illumination,
  shadows and lights and upscales in one step, in place of DLSS and the add-on's own denoisers, with a
  choice of NVIDIA's presets. The global illumination then takes a sample at every pixel, for sharper,
  quicker light, and the game's depth of field tells it where it blurs.
- **SDR: no more banding in smooth fog and sky.** The game kept its picture in 8 bits from early in
  the frame, so smooth fog showed bands a level or two apart. The picture now stays in 16-bit
  colour until the end in SDR too, as with HDR, and a fine dither hides the last rounding. It costs
  about 0.3 ms at 4K.
- **Global illumination: light keeps bouncing.** The global illumination's light cache stopped after
  the third surface. It now passes its own bounced light on every frame, so light bounces until it
  fades. Its third bounce also counted only the rays that hit something, which made it too bright, most
  in open ground. Bright, closed rooms gain the most; elsewhere the change is slight.
- **Global illumination: steadier light along edges and in grass.** With DLSS or FSR jittering the
  frame, the global illumination's denoiser started the light over along object edges every frame,
  most in grass and bushes. Its half-resolution samples now stay on the surface they had the frame
  before.
- **Global illumination: the denoiser follows the camera exactly.** Its idea of where each pixel was
  in the last frame was off by up to a fifth of a pixel near the camera, a little differently with
  every frame's jitter. It is now worked out relative to the camera, in double precision.
- **Diagnostics: the denoiser's own checks as a debug view.** **Global illumination denoiser (NRD)**
  shows NVIDIA's validation layer for the global illumination's denoiser: its view depth, normals,
  motion against a still scene and frames of history.
- **Ray tracing: the helicopter's cabin has its walls.** Between missions the rays passed through the cabin's
  walls, as it is held apart from the rest of the game's models: sunlight flooded in, and with path tracing the
  cabin was about four times as bright as the game's own. It is now dark inside, with daylight through its windows.
- **Ray-traced shadows: no lit halo around sunlit characters.** A character in the sun in front of a
  shaded wall left a lit band on the wall along their outline, wider the lower the render scale. The
  shadow denoiser no longer mixes a pixel's shadow with what lies in front of or behind it.
- **Ray-traced shadows no longer flicker along edges.** With DLSS or FSR jittering the frame, the
  shadow denoiser often dropped a pixel's history along object edges and showed the raw, noisy rays
  there. It now keeps the history from the last frame's pixels on the same surface.
- **Motion blur: a moving character's outline no longer steps with upscaling.** With the game's motion
  blur on and DLSS or FSR upscaling, a character walking past showed staircase steps along their
  outline, 2-3 px wide at Quality and 5-6 px at Ultra Performance in 4K. The blur's velocity and depth
  are now prepared at the same size the blur draws at, so the steps match native resolution's, except
  at Ultra Performance, where they halve to the render pixel.
- **Motion blur: the Strength slider works.** It never changed the blur. It now scales how far the
  blur reaches, from none at 0% to the game's own at 100%; above 100% it reaches further up to the
  game's limit.
- **Motion blur is halved while frame generation runs**, as NVIDIA recommends: each frame shown covers
  half the motion.

## 1.9.0

This release focuses on shadows, with a lot of it built by ZioPao.

- **HDR: the sun is a disc again, not a ring.** With HDR on, the sky's sun was drawn as a bright
  ring around a dimmer centre. The game's sky and the physical sky now keep the game's limit of 1,
  so the sun stays a solid disc.
- **The contact shadow sliders sit below both of their toggles.** Contrast, reach, thickness and fade
  belong to the sun's contact shadows and the lamps' alike, so they now end the Shadows page, after
  the lamps' toggle, instead of sitting inside the sun's.
- **Lamp contact shadows count a caster within their own 5 mm, not Contact shadow thickness.** That
  setting is a share of the depth left to the far plane, which grows with the distance: past 1% it
  reaches the bound a caster may sit in and leaves no window at all, so the term silently died for
  receivers past 10 m, and below that raising it narrowed the march where it widens the sun's trace.
  The lamps now use a fixed 5 mm, and the setting shapes the sun's contact shadows alone.
- **Lamp contact shadows read the depth buffer four texels at a time.** The march took one depth a
  step, forty of them a pixel; a gather returns the four texels inside a two-pixel square for one
  instruction, which is the ground two steps cover, so the same span now takes half the requests and
  makes twice the comparisons. The caster window is compared in the buffer's own depth (`A + B / z`)
  rather than per-texel view depth, so the two conversions a pair costs are shared by its four
  samples. The image is the same span of shadow, sampled more finely.
- **Lamp contact shadows skip what the light cannot light.** The march ran over every pixel a lamp's
  volume covers, including the ones whose surface faces away from the light: the game clamps
  `dot(normal, light)` at zero and scales both its diffuse and its specular by it, so those pixels get
  no light and the shadow written for them is never read. They now return after one read of the
  G-buffer's normal, and a lamp whose reach cannot touch the screen keeps the game's draws.
- **Lamp contact shadows draw only the far faces of a light's volume.** The shadow draw drew both
  faces of a closed volume and threw one result away. It now draws the far faces, as the game's
  lighting draw does, so it covers the pixels that read the shadow at half the cost. The image is
  unchanged.
- **Lamp contact shadows cost a fraction of what they did.** Each lamp traced Bend Studio's
  screen-space shadows in a compute pass over its whole screen rectangle, 120 samples a pixel, and
  then drew its volume a second time to put the result in the light targets. The lamps now march the
  depth buffer inside that draw: about 40 steps a pixel crowded near the surface, a caster counted
  only within a quarter of a metre of the ray, and pixels the light cannot reach left without a
  step. A lamp wholly past the contact shadows' fade keeps the game's draws. The sun keeps Bend's trace, which costs one to eight dispatches for each recording of
  the depth it reads.
- Fix a fine grid pattern over dark areas, worst at night and in fog with upscaling: the game added a
  fixed 8x8 dither to its sky, its foggy scene and its glass before DLSS, which enlarged it with the
  image. The dither is taken out of those shaders, and out of the physical sky. In SDR, smooth fog
  can now show slight banding (#54); HDR has none.
- Add **Contact shadow contrast**, **reach**, **thickness** and **fade** settings, under the shadows:
  how dark and how hard the trace is (0.5 to 4, 1 by default), how far along the screen it marches
  (8 to 120 pixels, 80 by default), how deep a surface counts as a contact (0.2% to 2.5% of its depth
  to the far plane, 0.5% by default) and how far away it still shows (50 to 700 m, 500 by default,
  full strength to 60% of that). All apply to the sun's and the lamps'.
- Contact shadows now fade out with distance: before, one at 100 m was as sharp and as strong as the
  one underfoot, and now the trace thins out and leaves the shadow map to it.
- Reorganise the **Shadows** page: the ray-traced lights sit beside the ray-traced shadows, and the
  contact shadows have a heading of their own under the soft shadows, with their two sliders.
- Add **Lamp soft shadows (PCSS)**: point and spot lights with a shadow map filter it with
  contact-hardening soft shadows instead of the game's four evenly sharp comparisons, so a lamp's
  shadow is sharp where it meets a surface and widens with distance. Off by default; rebuilt copies
  of `SSLighting2_Point` and `SSLighting2_Spot`.
- Foliage (grass, bushes, tree leaves) casts screen-space contact shadows: the trace no longer
  skips the foliage material, so clumps shade themselves.
- Fix the game's motion blur looking pixelated while upscaling. It blurred a small copy of the image
  from before DLSS (a third of the screen's width at Quality) and stretched it over the screen. It
  now blurs DLSS's image at half the screen's size, as it does without upscaling.
- With HDR, motion blur keeps highlights bright while the camera moves, at every upscaler setting:
  the game blurred through an 8-bit copy, which clipped them at white.
- Add **Motion blur** to the Display page: the game's own blur, now drawn with the upscaler on too,
  with a **Strength** slider (0% to 200% of what the game's setting asks for). Off leaves it out
  whatever the game's setting says.
- Ray-traced shadows:
  - Sunlight no longer comes through building walls indoors. Solid stand-ins the game casts in
    place of main-view-only walls block the sun's rays again; lamps' rays still pass them.
  - Hair lets light through in part: each layer a ray crosses keeps half its light, so a thin
    fringe casts a light shadow and thick hair a dark one, instead of every hair card casting a
    solid black one.
  - Scaffold tarps cast shadows, through the solid stand-in the game uses for them.
  - Models the game has faded out are left out of the scene. A building shell at Mother Base, never
    drawn, turned the wall behind it black up close with global illumination.

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
