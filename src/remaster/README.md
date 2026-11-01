# Remastering tool

Tunes the game's materials in the running game and saves the result into the repository, so that every
build ships it. For example: a lamp whose surface is too bright goes down without touching any other.

It has two halves. This folder holds the rules, which every build ships. `src/dev/remaster/` holds the tool that writes
them, which only a dev build has.

Game facts behind it: `docs/DEVELOPMENT.md`, "Materials".

## Using it

In a dev build, two keys, with no ReShade window involved:

- **F6** while playing: a mark. It keeps the map, your position, the time of day and every material drawn in the last
  second. Nothing stops, so mark and keep walking.
- **F7**: the panel at the right edge. The game keeps running. Hold the right mouse button to walk and look
  around with it open. **Esc** or **F7** closes it.

Then work down the marks:

1. Pick a mark. **Go there** warps you back and sets the time of day, travelling first if it is another map. A story
   mission comes back as that place in its map's free roam. **Done** takes the mark off the list.
2. Hover a material: it flashes in the game. Click it to tune it.
3. Drag **Brightness**, or roll the wheel over it. **Colour** opens a picker, and **More parameters** shows the rest.
4. Hold **C** or **Hold for original** to see the game's value for this material alone.
5. Name it, then press **Save**. That writes `src/remaster/materials.json` in the checkout the build came from.
   Commit that file.

**Rules** lists every rule, with those whose material the game has not loaded here dimmed. **Back to original** takes a
rule away.

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

## What it reaches

- The rasterised image: at once.
- The rays: self-lit light only, read from the model file, so a change reaches them at the next start. A material
  under an intensity of 10 stays out of the rays, whatever a rule makes it.
- Not a lamp's light on the room. That is a separate game light.
- Not surface colour or roughness: textures hold those, not constants.
- Not every glow. Some use no `SelfColorIntensity`: look under **All**.

## Detaching it

To take out the tool alone, detach the development tools (`src/dev/README.md`): the rules go on working from the file
built into the add-on.

To take out both, delete this folder, `src/dev/remaster/` and `tests/remaster_tests.cpp`, then remove:

| File | Lines |
| --- | --- |
| `CMakeLists.txt` | `include(src/remaster/remaster.cmake)` |
| `src/addon.cpp` | `log(remaster::hook());` and the include |
| `src/modules.cpp` | The `remaster::configure` row and the include |
| `src/dev/dev.cpp` | The `remaster::drawTool()` line and the include |
| `src/dev/dev.cmake` | The `src/dev/remaster/` sources and `PHANTOM_FX_MATERIALS_FILE` |
| `src/dev/tests.cmake` | `marks_tests` |
| `src/passes/ray_tracing/engine_scene.cpp` | The last argument of `describeModel` and the include |
| `docs/DEVELOPMENT.md` | The `src/remaster/` rows, "Materials", and the mentions under "Build" and "Package a release" |

That leaves the add-on working. Two small things only this tool uses stay behind, to remove as well for a clean
tree: `SelfLightFilter` in `scene_description` with its test `tunedSelfLight`, and `Configuration::areEnhancementsOn`.
