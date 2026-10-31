# Remastering tool

Tunes the game's materials in the running game and saves the result into the repository, so that every
build ships it. For example: a lamp whose surface is too bright goes down without touching any other.

Game facts behind it: `docs/DEVELOPMENT.md`, "Materials".

## Using it

In a dev build, open the overlay, then **Developer** > **Materials**.

1. **Show** picks the kind of material. **Self-lit** lists brightest first.
2. **Drawn now** keeps the list to what the game drew in the last second.
3. Pick a material. It blinks on screen, so you can see it is the right one. Press **Blink on screen** to stop.
4. Move its sliders. The change shows on the next frame.
5. Give it a label, then press **Save**. That writes `src/remaster/materials.json` in the checkout the
   build came from. Commit that file.

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

## What it reaches

- The rasterised image: at once.
- The rays: self-lit light only, read from the model file. It takes effect when the game next loads the model.
- Not a lamp's light on the room. That is a separate game light.
- Not surface colour or roughness: textures hold those, not constants.
- Not every glow. Some use no `SelfColorIntensity`: look under **All**.

## Detaching it

Everything lives in this folder. To take it out, delete the folder and `tests/remaster_tests.cpp`, then
remove:

| File | Lines |
| --- | --- |
| `CMakeLists.txt` | `include(src/remaster/remaster.cmake)` |
| `src/addon.cpp` | `log(remaster::hook());` and the include |
| `src/modules.cpp` | The `remaster::configure` row and the include |
| `src/menu.cpp` | `remaster::drawTool();` and the include |
| `src/passes/ray_tracing/engine_scene.cpp` | The last argument of `describeModel` and the include |

`scene_description`'s `SelfLightFilter` and `Configuration::enhancements` know nothing of this tool, and can stay.
