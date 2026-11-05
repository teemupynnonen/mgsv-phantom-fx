# Remastering tool

Tunes the game's materials and lamps in the running game and saves the result into the repository, so that every
build ships it. For example: a lamp whose surface is too bright goes down without touching any other, and so does the
light it throws on the room.

It has two halves. This folder holds the rules, which every build ships. `src/dev/remaster/` holds the tool that writes
them, which only a dev build has.

Game facts behind it: `docs/DEVELOPMENT.md`, "Materials".

## Using it

In a dev build, two keys, with no ReShade window involved:

- **F6** while playing: a mark. It keeps the map, your position, the time of day, every material drawn in the last
  second and every lamp lit in it. Nothing stops, so mark and keep walking.
- **F7**: the panel at the right edge. The game keeps running. Hold the right mouse button to walk and look
  around with it open. **Esc** or **F7** closes it.

Then work down the marks:

1. Pick a mark. **Go there** warps you back and sets the time of day, travelling first if it is another map. A story
   mission comes back as that place in its map's free roam. **Done** takes the mark off the list.
2. **Lights** lists the mark's lamps (the light on the room), then its glowing materials (how a light itself looks),
   each brightest first by its value at the mark: a lamp's light at a metre and a glow's intensity are not the same
   measure. **Glass** and **All** list the other materials. Hover one: it flashes in the game. Click it, or walk
   the list with **Up** and **Down**.
3. Drag **Brightness**, roll the wheel over it, or press **[** and **]** to step it by a quarter; **0** puts the light
   out and **R** takes its rule away. **Colour** opens a picker, and **More parameters** shows the rest. A lamp has
   **Brightness** alone: its light times that.
4. Hold **C** or **Hold for original** to see the game's value for this material or lamp alone. Hold **V** to see the
   game without any rule at all, lamps, materials and the rays' glow alike.
5. **Ctrl+Z** undoes any change, **Ctrl+Y** (or **Ctrl+Shift+Z**) redoes it. A drag or a run of the same key is one
   step.
6. A new rule gets a label that says where and what, as "Quarantine platform, lamp 1379 at -1.5, 3.2, 20.9". Say what
   it is for instead; the head counts the rules still under the tool's label. Then press **Save**. That writes
   `src/remaster/materials.json` in the checkout the build came from. Commit that file.

**Rules** lists the lamps' rules, then the materials', with those whose material the game has not loaded here dimmed.
**Unsaved**, **Saved** and **All** at its top pick which show. An unsaved rule says how it differs from the file (new,
changed or removed), and **Revert** puts it back as the file has it. **Remove** takes a saved rule away. **Discard**
in the head puts every rule back as the file has it, after asking. Ctrl+Z undoes a revert or a discard like any change.

The rules are off with the visual enhancements (F1), and the panel waits until they are back on.

A dev build reads that file at start. Other builds read the copy built into the add-on. Marks are scratch: they stay
in `PhantomFX-marks.json` beside the add-on, not in the repository.

## Rules

```json
{
  "materials": [
    {
      "label": "Corridor ceiling lamp",
      "material": "e45333b9516f",
      "parameters": {
        "SelfColorIntensity": { "from": [221182.0], "to": [40000.0] }
      }
    }
  ]
}
```

- `material` is the StrCode64 of the material's name, twelve hex digits.
- `parameters` go by name where it is known (`KnownParameters` in `rules.cpp`), else by hash.
- `from` and `to` are one to four components, from x on.

A rule applies to a material of that name that holds **every** `from`. Names are not unique: `ca117249330f`
names lamps of 221182, 100000 and 600 in one mission, and `from` tells them apart. It also means that a
value the game animates passes untouched, and that a rule stops matching rather than doing harm if the
game's data ever differs.

Materials the game has merged are one material. Two lamps that share a name, shader, textures and values
are tuned together.

Going back works by value too, so pick a `to` that no other material of the name holds. If one does, F1
takes it for a tuned one and sets it to `from`, and the tool lists it as tuned.

## Lamps

```json
{
  "materials": [],
  "lamps": [
    { "label": "Corridor spot", "location": 55, "at": [-1.4912, 3.1734, 20.9307], "brightness": 0.3 }
  ]
}
```

The game has no name for a lamp, so a lamp is its map (`vars.locationCode`) and its place in the world: a lamp drawn
within 5 cm of `at` is the rule's. `brightness` multiplies its light, 0 puts it out. A lamp that moves is never matched.

The rule goes on where the add-on records the lamp's light draw (`local_lights.cpp`), so it reaches the lamp's light,
its shadows, path tracing and the light it bounces. That needs ray-traced lights or path tracing on: without either,
the game draws its lamps itself and the rule does nothing. The game's exposure measures that light too, so putting out
a room's main lamp can make the rest of the frame brighter as the exposure opens up, as it would in the game.

## What it reaches

- The rasterised image: at once.
- The rays: self-lit light, at once. The scene keeps each self-lit mesh's values from its model file and tunes them
  again whenever the rules change (`SceneContents::retuneSelfLight`). A material under an intensity of 10 in its file
  stays out of the rays, whatever a rule makes it.
- A lamp's light on the room: see "Lamps".
- Not surface colour or roughness: textures hold those, not constants.
- Not every glow. Some use no `SelfColorIntensity`: look under **All**.
- Not what draws outside the hooked draws. A mark takes only materials drawn through them, so a material can be missing
  from one. The automation's `remaster loaded` lists every self-lit material loaded.

A self-lit intensity is what the surface looks like times the exposure, so a bright one stays white long after you
start lowering it: a quarantine platform ceiling tube of 221,182 stayed white down to about 200. **Brightness** goes down
to a millionth of the original for that.

## Detaching it

To take out the tool alone, detach the development tools (`src/dev/README.md`): the rules go on working from the file
built into the add-on.

To take out both, delete this folder, `src/dev/remaster/` and `tests/remaster_tests.cpp`, then remove:

| File | Lines |
| --- | --- |
| `CMakeLists.txt` | `include(src/remaster/remaster.cmake)` |
| `src/addon.cpp` | `log(remaster::hook());` and the include |
| `src/features/ray_traced_lights/local_lights.cpp` | The `else` block that calls `remaster::lampBrightness`, and the include |
| `src/passes/ray_tracing/ray_tracing.cpp` | `frame.selfLight` and `frame.selfLightVersion`, and the include |
| `src/dev/automation.cpp` | The `remaster` line and the include |
| `src/modules.cpp` | The `remaster::configure` row and the include |
| `src/dev/dev.cpp` | The `remaster::drawTool()` line and the include |
| `src/dev/dev.cmake` | The `src/dev/remaster/` sources and `PHANTOM_FX_MATERIALS_FILE` |
| `src/dev/tests.cmake` | `marks_tests` |
| `docs/DEVELOPMENT.md` | The `src/remaster/` rows, "Materials", and the mentions under "Build" and "Package a release" |

That leaves the add-on working. Two small things only this tool uses stay behind, to remove as well for a clean
tree: `SelfLightFilter` in `scene_description` with its test `tunedSelfLight`, `SceneContents::retuneSelfLight` with the
`selfLight` fields of `GameScene` and `gi_renderer::Frame`, and `Configuration::areEnhancementsOn`.
