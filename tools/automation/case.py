"""Says what is wrong in a debug case (src/automation.hpp, docs/DEVELOPMENT.md "Debug cases").

  case.py [--copy] [<case>]     report.md, numbers.json and pictures in the case's folder; the latest case by default

Three checks, each against what the game itself says of the same frozen frame:
- the scene: does the ray-traced scene have the surface the game drew at each pixel?
- the sun: do the rays' shadows agree with the game's shadow map?
- the light: how far is the traced frame from the game's own?
Each finding is a region of the frame with a pixel to probe and, from the revealed scene, the model in it
and why the scene left it out.
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

import fx

HEADER = np.dtype([("magic", "S4"), ("width", "<u4"), ("height", "<u4"), ("frame", "<u4"), ("inverse", "<f4", (16,)), ("eye", "<f4", (4,)), ("sun", "<f4", (4,))])
PIXEL = np.dtype([("view", "<u4"), ("view_distance", "<f4"), ("surface", "<u4"), ("surface_distance", "<f4"), ("sun", "<u4"), ("sun_distance", "<f4"),
    ("game_distance", "<f4"), ("kinds", "<u4")])

# As GI_INSPECT_SURFACE_* in gi_shared.h.
SURFACE_METRES, SURFACE_SHARE = 0.05, 0.02
# Regions under this share of the frame are noise: alpha-tested edges and the shadow map's texels.
LEAST_REGION = 0.0005
MOST_REGIONS = 6
MOST_NAMES = 4
LEFT_OUT = ("[left out", "[dropped")  # scene_contents.cpp's marks on what only a revealed scene holds
# How far a scenario's numbers may move before it counts as worse: runs of the same spot differ by a few tenths
# of a point (a windmill turns, smoke drifts). Percentages of the frame, the larger of the two; the light, either way.
WORSE_POINTS, WORSE_SHARE, WORSE_LIGHT = 1.0, 0.25, 1.15
# A run did not reach a scenario's view with the eye further off, or under this share of its surfaces within a
# quarter of their distance. Not nearer: a frame's jitter moves ground at a shallow angle more than the scene check's
# tolerance, and Afghanistan's ground and grass read 89% that way with the eye exactly in place (2026-10-30).
MOST_EYE_METRES, LEAST_SAME_VIEW, SAME_VIEW_SHARE = 0.1, 90, 0.25
OVERLAY = {"lacks": (255, 0, 255), "in front": (255, 40, 40), "in the sky": (255, 230, 0), "extra shadow": (255, 40, 40), "missing shadow": (0, 230, 255)}


class Record:
    """A frame's record of every pixel (GIInspectPixel) and what its hits are."""

    def __init__(self, path):
        data = path.with_suffix(".bin").read_bytes()
        self.header = np.frombuffer(data, HEADER, 1)[0]
        self.height, self.width = int(self.header["height"]), int(self.header["width"])
        self.pixels = np.frombuffer(data, PIXEL, offset=HEADER.itemsize).reshape(self.height, self.width)
        self.names = {}
        table = path.with_suffix(".tsv")
        if table.exists():
            for line in table.read_text(errors="replace").splitlines():
                hit, _, name = line.split("\t", 2)
                self.names[int(hit)] = name

    def count(self, field, mask):
        """What `field`'s hits are over `mask`, the same names together, most pixels first."""
        hits, counts = np.unique(self.pixels[field][mask], return_counts=True)
        named = {}
        for hit, count in zip(hits, counts):
            if hit:
                name = self.names.get(int(hit), f"hit {hit}")
                named[name] = named.get(name, 0) + int(count)

        return sorted(named.items(), key=lambda item: -item[1])


def regions(mask, opening):
    """The large connected regions of `mask`, biggest first, thin lines opened away."""
    cleaned = ndimage.binary_opening(mask, iterations=opening) if opening else mask
    labels, count = ndimage.label(cleaned)
    if not count:
        return []

    sizes = ndimage.sum_labels(cleaned, labels, range(1, count + 1))
    least = LEAST_REGION * mask.size
    found = []
    for index in np.argsort(-sizes)[:MOST_REGIONS]:
        if sizes[index] < least:
            break

        region = labels == index + 1
        ys, xs = np.nonzero(region)
        # A pixel of the region itself near its middle, for the probes.
        nearest = np.argmin((xs - xs.mean()) ** 2 + (ys - ys.mean()) ** 2)
        found.append({"mask": region, "pixels": int(sizes[index]), "centre": (int(xs[nearest]), int(ys[nearest])),
            "box": (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))})

    return found


def named(names, total):
    if not names:
        return ["nothing the revealed scene holds: the game draws it from a list the add-on does not read"]

    lines = [f"{count * 100 / total:.0f}%: {name}" for name, count in names[:MOST_NAMES]]
    if len(names) > MOST_NAMES:
        lines.append(f"and {len(names) - MOST_NAMES} more")

    return lines


def left_out_first(names):
    return sorted(names, key=lambda item: (not any(tag in item[0] for tag in LEFT_OUT), -item[1]))


def region_text(letter, title, region, frame_pixels, details):
    x, y = region["centre"]
    left, top, right, bottom = region["box"]
    lines = [f"**{letter}. {title}: {region['pixels'] * 100 / frame_pixels:.1f}% of the frame**, pixels {left},{top} to {right},{bottom}; probe with `fx.py probe {x} {y}`"]
    lines += [f"- {detail}" for detail in details]

    return lines + [""]


def scene_check(scene, revealed, numbers):
    game = scene.pixels["game_distance"]
    drawn = game > 0
    tolerance = SURFACE_METRES + SURFACE_SHARE * game
    has_view = scene.pixels["view"] != 0
    in_front = drawn & has_view & (scene.pixels["view_distance"] < game - tolerance)
    lacks = drawn & (scene.pixels["surface"] == 0) & ~in_front
    in_sky = ~drawn & has_view
    frame = game.size
    matched = drawn & ~in_front & ~lacks
    numbers.update({"scene_lacks": lacks.sum() * 100 / frame, "in_front": in_front.sum() * 100 / frame, "in_sky": in_sky.sum() * 100 / frame})
    lines = ["## The scene against the game's surfaces", "",
        f"{matched.sum() * 100 / max(drawn.sum(), 1):.1f}% of the game's surfaces are in the scene. It lacks {lacks.sum() * 100 / frame:.1f}% of the frame, "
        f"has something in front of {in_front.sum() * 100 / frame:.1f}%, and in the game's sky over {in_sky.sum() * 100 / frame:.1f}%.", ""]

    found = {"lacks": [], "in front": [], "in the sky": []}
    letters = iter("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
    for label, mask, opening in (("lacks", lacks, 1), ("in front", in_front, 1), ("in the sky", in_sky, 1)):
        for region in regions(mask, opening):
            region["letter"] = next(letters)
            found[label].append(region)
            area = region["mask"]
            distance = float(np.median(game[area])) if label != "in the sky" else float(np.median(scene.pixels["view_distance"][area]))
            if label == "lacks":
                details = [f"the game's surface is {distance:.1f} m away; the scene has nothing there"]
                if revealed:
                    details += ["revealed, it is: " + line for line in named(left_out_first(revealed.count("surface", area)), region["pixels"])]
            else:
                details = [f"{distance:.1f} m away, what the scene has there:"] + named(scene.count("view", area), region["pixels"])

            lines += region_text(region["letter"], f"the scene {label}" if label == "lacks" else f"something {label}", region, frame, details)

    return lines, found


def sun_check(case, scene, revealed, numbers):
    path = case / "game-shadow.dds"
    lines = ["## The sun's rays against the game's shadow map", ""]
    found = {"extra shadow": [], "missing shadow": []}
    if not path.exists():
        return lines + ["No reference: the frame had no shadow-mapped sun pass (indoors, or past the game's shadow range).", ""], found

    game_shadow, _ = fx.texture(path)
    if game_shadow.shape != scene.pixels.shape:
        return lines + ["No reference: the game's shadow is at another size than the record.", ""], found

    answered = (game_shadow >= 0) & (scene.pixels["game_distance"] > 0)
    if not answered.any():
        return lines + ["No reference: the game's shadow map answered for no pixel.", ""], found

    ray_lit = scene.pixels["sun"] == 0
    map_lit = game_shadow >= 0.5
    extra = answered & ~ray_lit & map_lit
    missing = answered & ray_lit & ~map_lit
    frame = answered.size
    numbers.update({"extra_shadow": extra.sum() * 100 / frame, "missing_shadow": missing.sum() * 100 / frame})
    lines += [f"The game's map answers for {answered.sum() * 100 / frame:.1f}% of the frame. The rays add shadow over {extra.sum() * 100 / frame:.1f}% of it and lose "
        f"shadow over {missing.sum() * 100 / frame:.1f}%.", ""]

    letters = iter("STUVWXYZabcdefgh")
    for label, mask in (("extra shadow", extra), ("missing shadow", missing)):
        for region in regions(mask, 2):
            region["letter"] = next(letters)
            found[label].append(region)
            area = region["mask"]
            if label == "extra shadow":
                distance = float(np.median(scene.pixels["sun_distance"][area]))
                details = [f"the rays are blocked {distance:.2f} m away by:"] + named(scene.count("sun", area), region["pixels"])
            else:
                details = ["the rays reach the sun here, the game's map says blocked"]
                if revealed:
                    details += ["revealed, what blocks them: " + line for line in named(left_out_first(revealed.count("sun", area)), region["pixels"])]

            lines += region_text(region["letter"], label, region, frame, details)

    return lines, found


def resized(light, width, height):
    channels = [np.asarray(Image.fromarray(light[..., c].astype(np.float32)).resize((width, height), Image.BOX)) for c in range(3)]
    return np.stack(channels, -1)


def light_check(case, scene, numbers):
    lines = ["## The light against the game's own", ""]
    traced_path, vanilla_path = case / "traced.dds", case / "vanilla.dds"
    if not traced_path.exists() or not vanilla_path.exists():
        return lines + ["No frames to compare.", ""], None, None

    traced = resized(fx.linear_light(traced_path), scene.width, scene.height)
    vanilla = resized(fx.linear_light(vanilla_path), scene.width, scene.height)
    luminance = np.array([0.2126, 0.7152, 0.0722])
    traced_light, vanilla_light = traced @ luminance, vanilla @ luminance
    drawn = scene.pixels["game_distance"] > 0
    floor = max(float(np.percentile(vanilla_light, 5)), 1e-3)
    ratio = (traced_light + floor) / (vanilla_light + floor)
    if drawn.any():
        numbers["light"] = float(np.median(ratio[drawn]))
        lines += [f"Where the game drew something, the traced frame is {np.median(ratio[drawn]):.2f}x the game's (median); "
            f"{(ratio[drawn] > 2).mean() * 100:.1f}% of it over 2x, {(ratio[drawn] < 0.5).mean() * 100:.1f}% under 0.5x.", ""]

    # The frame's rows and columns of cells, for a look at where.
    rows, columns = 4, 8
    lines += ["Median traced / game in a grid over the frame:", "", "| " + " | ".join(" " for _ in range(columns)) + " |", "|" + "---|" * columns]
    for row in range(rows):
        cells = []
        for column in range(columns):
            top, bottom = row * scene.height // rows, (row + 1) * scene.height // rows
            left, right = column * scene.width // columns, (column + 1) * scene.width // columns
            cell = ratio[top:bottom, left:right][drawn[top:bottom, left:right]]
            cells.append(f"{np.median(cell):.2f}" if cell.size else "sky")
        lines.append("| " + " | ".join(cells) + " |")

    return lines + [""], traced, vanilla, ratio


def overlay(base, found, path):
    picture = (base * 0.55).astype(np.uint8)
    for label, regions_of_kind in found.items():
        for region in regions_of_kind:
            picture[region["mask"]] = (picture[region["mask"]] * 0.4 + np.array(OVERLAY[label]) * 0.6).astype(np.uint8)

    image = Image.fromarray(picture)
    draw = ImageDraw.Draw(image)
    for regions_of_kind in found.values():
        for region in regions_of_kind:
            draw.rectangle(region["box"], outline=(255, 255, 255))
            draw.text((region["box"][0] + 4, region["box"][1] + 2), region["letter"], fill=(255, 255, 255))
    image.save(path)


def ratio_picture(ratio, path):
    """Traced over vanilla: red brighter, blue darker, full at 4x either way, grey the same."""
    amount = np.clip(np.log2(ratio) / 2, -1, 1)
    picture = np.full((*ratio.shape, 3), 128.0)
    picture[..., 0] += np.maximum(amount, 0) * 127 - np.maximum(-amount, 0) * 128
    picture[..., 2] += np.maximum(-amount, 0) * 127 - np.maximum(amount, 0) * 128
    picture[..., 1] -= np.abs(amount) * 128
    Image.fromarray(picture.astype(np.uint8)).save(path)


def write(case):
    case = Path(case)
    state = {}
    if (case / "state.txt").exists():
        state = dict(line.split("=", 1) for line in (case / "state.txt").read_text().splitlines() if "=" in line)

    log = (case / "log.txt").read_text() if (case / "log.txt").exists() else ""
    errors = [line for line in log.splitlines() if line.startswith("error:")]
    if not (case / "scene.bin").exists():
        report = "\n".join([f"# Debug case {case.name}", "", f"No record of the frame: mission {state.get('mission', '?')}, frame {state.get('frame', '?')}.", ""]
            + [f"- {line}" for line in errors] + ["", "A freeze in a loading screen renders nothing, and with the ray tracing off there is no scene."]) + "\n"
        (case / "report.md").write_text(report, encoding="utf-8")
        return report

    scene = Record(case / "scene")
    revealed = Record(case / "revealed") if (case / "revealed.bin").exists() else None
    if revealed and revealed.pixels.shape != scene.pixels.shape:
        revealed = None

    lines = [f"# Debug case {case.name}", "", f"Mission {state.get('mission', '?')}, camera {state.get('camera', '?')}, render {scene.width}x{scene.height}, frame {state.get('frame', '?')}.", ""]
    if errors:
        lines += ["Errors while recording:", ""] + [f"- {line}" for line in errors] + [""]

    numbers = {}
    scene_lines, scene_found = scene_check(scene, revealed, numbers)
    sun_lines, sun_found = sun_check(case, scene, revealed, numbers)
    light = light_check(case, scene, numbers)
    (case / "numbers.json").write_text(json.dumps({key: round(float(value), 3) for key, value in numbers.items()}, indent=2) + "\n")
    lines += scene_lines + sun_lines + light[0]
    lines += ["Pictures: `scene-check.png`, `sun-check.png`, `traced.png`, `vanilla.png`, `light-ratio.png`."]

    if light[1] is not None:
        traced, vanilla, ratio = light[1], light[2], light[3]
        base = fx.shown(traced)
        Image.fromarray(base).save(case / "traced.png")
        Image.fromarray(fx.shown(vanilla)).save(case / "vanilla.png")
        ratio_picture(ratio, case / "light-ratio.png")
    else:
        base = np.full((scene.height, scene.width, 3), 90, np.uint8)

    overlay(base, scene_found, case / "scene-check.png")
    overlay(base, sun_found, case / "sun-check.png")

    report = "\n".join(lines) + "\n"
    (case / "report.md").write_text(report, encoding="utf-8")

    return report


def same_view(first, second):
    """How nearly two cases show the same frame, the game's own surfaces in each compared pixel by pixel: the
    share that match, percent, and how far apart the eyes are; none without records of one size."""
    if not (Path(first) / "scene.bin").exists() or not (Path(second) / "scene.bin").exists():
        return None

    a, b = Record(Path(first) / "scene"), Record(Path(second) / "scene")
    if a.pixels.shape != b.pixels.shape:
        return None

    near, far = a.pixels["game_distance"], b.pixels["game_distance"]
    shown = (near > 0) | (far > 0)
    same = np.abs(near - far) <= SAME_VIEW_SHARE * np.maximum(near, far)

    return float(same[shown].mean() * 100), float(np.linalg.norm(a.header["eye"][:3] - b.header["eye"][:3]))


def worse(expected, measured):
    """What moved the wrong way past what runs differ by, as text; better numbers pass."""
    found = []
    for key, value in expected.items():
        now = measured.get(key)
        if now is None:
            found.append(f"{key}: not measured, expected {value}")
        elif key == "light":
            if not value / WORSE_LIGHT <= now <= value * WORSE_LIGHT:
                found.append(f"light {value:.2f}x -> {now:.2f}x")
        elif now > value + max(WORSE_POINTS, value * WORSE_SHARE):
            found.append(f"{key} {value:.1f}% -> {now:.1f}%")

    return found


def side_by_side(before, after, path, labels):
    left, right = Image.open(before).convert("RGB"), Image.open(after).convert("RGB")
    sheet = Image.new("RGB", (left.width + right.width + 8, max(left.height, right.height)), (20, 20, 20))
    sheet.paste(left, (0, 0))
    sheet.paste(right, (left.width + 8, 0))
    draw = ImageDraw.Draw(sheet)
    for x, label in zip((10, left.width + 18), labels):
        draw.rectangle((x - 4, 8, x + 8 + 8 * len(label), 26), fill=(0, 0, 0))
        draw.text((x, 12), label, fill=(255, 255, 255))
    sheet.save(path)


def main():
    arguments = sys.argv[1:]
    if arguments[:1] == ["--copy"]:
        fx.game_folder = fx.AUTOMATION_GAME
        arguments = arguments[1:]

    if arguments:
        case = fx.cases() / arguments[0]
    else:
        case = max((folder for folder in fx.cases().iterdir() if folder.is_dir()), key=lambda folder: folder.stat().st_mtime)

    sys.stdout.reconfigure(encoding="utf-8")
    print(write(case))
    print(f"In {case}")


if __name__ == "__main__":
    main()
