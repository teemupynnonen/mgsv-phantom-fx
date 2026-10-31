# Remastering tool

Tunes the game's materials in the running game and saves the result into the repository, so that every
build ships it. For example: a lamp whose surface is too bright goes down without touching any other.

Game facts behind it: `docs/DEVELOPMENT.md`, "Materials".

## Using it

In a dev build, open the overlay, then **Developer** > **Materials**.

1. **Show** picks the kind of material. **Self-lit** lists brightest first.
2. **Drawn now** keeps the list to what the game drew in the last second.
3. Pick a material. It blinks on screen, so you can see it is the right one. Press **Blink on screen** to stop.
4. Move its sliders. The change shows on the next frame. A slider's reset button takes that value back,
   and a rule with nothing left goes.
5. Give it a label, then press **Save**. That writes `src/remaster/materials.json` in the checkout the
   build came from. Commit that file.

The rules are off with the visual enhancements (F1), and the tool waits until they are back on.

A dev build reads that file at start. Other builds read the copy built into the add-on.

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

Everything that runs lives in this folder. To take it out, delete the folder and `tests/remaster_tests.cpp`,
then remove:

| File | Lines |
| --- | --- |
| `CMakeLists.txt` | `include(src/remaster/remaster.cmake)` |
| `src/addon.cpp` | `log(remaster::hook());` and the include |
| `src/modules.cpp` | The `remaster::configure` row and the include |
| `src/menu.cpp` | The `remaster::drawTool()` line and the include |
| `src/passes/ray_tracing/engine_scene.cpp` | The last argument of `describeModel` and the include |
| `docs/DEVELOPMENT.md` | The `src/remaster/` row, "Materials", and the mentions under "Build" and "Package a release" |

That leaves the add-on working. Two small things only this tool uses stay behind, to remove as well for a clean
tree: `SelfLightFilter` in `scene_description` with its test `tunedSelfLight`, and `Configuration::areEnhancementsOn`.
