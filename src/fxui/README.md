# fxui

A settings window for ReShade add-ons, built on the Dear ImGui that ReShade gives add-ons.

The window is a frame budget:

- **Top strip:** a master switch, the frame readout, and one rendered frame: the GPU's work split by effect, then the time the GPU sits idle.
- **Tabs:** one per section, or one for several sections, each then under its name. The search sits at their right.
- **Lines:** each item is one line: its name, its value (‹ On ›, or a choice), its cost and a fold mark. Clicked, it folds out to its summary, messages and settings; the one open before folds back.
- **Budget tab:** every effect with its state, cost, share of the frame and passes.

Every bar uses one scale: 1 ms is the same length everywhere.

## Add it to an add-on

Copy `src/fxui/` and add `fxui/fxui.cpp` to the add-on's sources. It needs ReShade's `include` and `imgui` folders on the include path, C++20, and nothing else.

## Each frame

1. Fill a `fxui::Budget`: one `Line` per effect whose GPU work you time.
2. Open a `fxui::Window`.
3. Draw your pages. Items show their settings while they are on.

```cpp
#include "fxui/fxui.hpp"

#include <imgui.h>
#include <reshade.hpp> // After imgui.h: routes ImGui calls through ReShade.

enum class Section { Lighting, Water, Budget, Diagnostics };

constexpr fxui::Section sections[]{
    {"Lighting", fxui::hue::Blue, false, "Graphics"}, // Has timed lines, so it gets a hue
    {"Water", fxui::hue::Violet, false, "Graphics"},  // Shares the Graphics tab, under its name
    {"Budget", 0, true},                              // The overview: a tab of its own
    {"Diagnostics"},
};

void drawSettings(Settings& wanted, const Costs& costs)
{
    fxui::Budget budget;
    budget.gpuMs = costs.gpuFrame;
    budget.frameMs = costs.frame;
    budget.lines.push_back({
        .id = "ao",
        .name = "Ambient occlusion",
        .tech = "GTAO",
        .section = static_cast<int>(Section::Lighting),
        .state = wanted.ao ? fxui::State::Working : fxui::State::Off,
        .ms = costs.ao,
        .passes = {{"March", costs.aoMarch}, {"Denoise", costs.aoDenoise}},
        .toggle = &wanted.ao,
    });

    const fxui::Window window(sections, budget, {.name = "My Mod"});

    if (const fxui::Page page(Section::Lighting); page)
        if (const fxui::Item item({.id = "ao", .summary = "Shade where surfaces meet."}); item)
            fxui::slider("Radius", wanted.aoRadius, {.min = 0.25f, .max = 4, .standard = 1, .format = "%.2f m"});

    if (const fxui::Page page(Section::Budget); page)
        fxui::ledger();
}
```

An item and a budget line with the same `id` belong together: the line gives the item its switch, state, cost and message, and its name and technical name where the item leaves them out.

Tabs follow the order of `sections`; on a tab of several, the sections show in the order their `Page`s are drawn. An item without a switch can put a choice on its line with `itemChoice()`, such as an upscaler's mode; its arrows step through the choices, and a click on it lists them.

## Building blocks

| Call | What it draws |
| --- | --- |
| `Window` | The strip, the frame bar, the tabs, and the page area |
| `Page` | A section, with its name on a shared tab |
| `Item`, `item()` | A line item that folds out, with or without settings; `isChanged` puts a dot after its name |
| `itemChoice` | A choice as the item's value, on its line |
| `slider` | Label, track, readout and a reset button while it differs from its default |
| `segmented` | Side-by-side buttons for a few choices, optionally with drawn icons; a reset with a `standard` |
| `combo`, `beginCombo`, `option`, `endCombo` | A dropdown for longer lists; a reset with a `standard` |
| `searchCombo` | A dropdown with a search box, for very long lists |
| `checkbox`, `button` | The usual; the checkbox has a reset with a `standard` |
| `hint`, `note`, `statusLine`, `fact`, `subheading`, `link` | Text in the window's styles |
| `ledger`, `costRow` | The budget table, and rows for other cost tables |
| `CostOverlay` | The costs in a corner over the game, for the `reshade_overlay` event |
| `toast`, `drawToast` | A short message over the game for a few seconds, such as "Frame captured". Call `toast` from anywhere; call `drawToast` in the `reshade_overlay` event, with a side or corner of the screen (default `Right`) |

## Window sizes

The layout follows the window's width, in pixels at the 13 px font:

- **Tabs:** where they and a 140 px search do not fit side by side, a dropdown of the tabs takes their place.
- **Lines:** below 520, a line drops its cost bar and keeps the milliseconds; the value narrows.
- **Settings** stack: the label and value on one line, the control full width below.
- The budget drops its share column and shows each state as a dot.

## States

`Working`, `Off`, `Held` (off by the master switch), `Replaced` (by the line in `replacedBy`), `Restart`, `NotRunning` and `Unavailable`. Set `message` to say why an effect is not running.

## Rules that keep the look

- Put every setting inside an item. During a search, only items show, every match folded out.
- Give sections with timed lines a hue from `fxui::hue`. Leave the others at 0.
- Use plain names. Put the technique's name in `tech`, such as "GTVBAO".
- Keep summaries to one line. Long explanations go in `tip`.
- One window per add-on: fxui keeps its tab, search, open item and scroll position between frames.
