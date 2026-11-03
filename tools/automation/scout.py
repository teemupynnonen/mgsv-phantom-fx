"""Scouts shots for a Phantom FX showcase in the automation copy: each shot is a place, a camera, a time of day
and a weather, taken with Phantom FX on and off and kept in a shot list, so a later session can go back to it.

  scout.py shot <name> <x> <y> <z> (<yaw> <pitch> | --look <x> <y> <z>) [<sky>] [--mm <focal length>] [--note "<why>"]
  scout.py again <name> [--clock <hh:mm>] [--weather <type>] [--as <new name>]
  scout.py survey <name> <x> <y> <z> [--radius <m>] [--height <m>] [--count <n>] [--outward <pitch>] [--clearance <m>] [<sky>] [--mm ..]
  scout.py times <name> <x> <y> <z> (<yaw> <pitch> | --look <x> <y> <z>) --clocks <hh:mm>,... [<sky>] [--mm ..]
  scout.py map <name> <x> <y> <z> [--radius <m>]   a top-down picture with world coordinates on it
  scout.py spots [<outpost>]       outposts' middles, or one outpost's soldiers: places on the ground
  scout.py restart                 a fresh copy on this checkout's build and its current settings, into free roam
  scout.py timelapse <shot> <from hh:mm> <to hh:mm> [--seconds <s>] [--as <name>]
  scout.py list

  <sky> = [--clock <hh:mm>] [--weather sunny|cloudy|rainy|sandstorm|foggy] [--player <x> <y> <z>]
  `--ab 1` on survey or times also takes each view as vanilla, beside Phantom FX's, with how much it changed

`survey` circles a place, looking at it from `count` sides (8 by default, the first from the south, -z), and puts
the views on one contact sheet, <name>-survey.png, each labelled with its camera for `shot`. Every camera stays
CLEARANCE metres over the highest surface near it, read from a top-down view first (Heights). With `--outward <pitch>`
it stands at the place instead and looks out to each side, the first north (+z). `times` puts one camera at
several times of day on a sheet. `timelapse` holds a saved shot's camera live (the game runs under it) and
speeds the game's clock so it goes from one time to the other in `seconds` (10); about six pictures a second go
into <name>-timelapse.webp, to judge whether the lighting keeps up. The clock wraps past midnight. A frozen game
keeps its sky: the clock moves, the lighting does not.

The camera is photo mode's: radians, yaw 0 looks along +z, turning towards +x, pitch positive upwards. Its lens is
`--mm` millimetres, 21 without saying (the game's own while the player stands), as the game's lens changes with
what the player does. The game streams its detail around the player, not the camera, so `--player` warps the
player near the shot first, on the same map; keep the player out of the frame. A warp below the ground drops the
player through the world, so warp to where a soldier stands (`spots`).

Pictures go to PhantomFX-automation/scout in the copy: <name>-on.png, <name>-off.png, <name>-split.png and
<name>-ab.png (vanilla left, Phantom FX right). The list is shots.json beside them.
"""
import json
import math
import shutil
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import fx

WEATHERS = ["sunny", "cloudy", "rainy", "sandstorm", "foggy"]
# The focal length without --mm: the game camera's own lens while the player stands.
LENS = 21
# Seconds the ray tracing takes to warm up: after Phantom FX is switched back on, and after the camera jumps (its
# lighting history starts again). The user measured 3 to 5 s for the switch.
WARM_UP, AFTER_JUMP = 5, 4
# Seconds the vanilla frame needs after the switch, and the weather to blend in.
VANILLA, SETTLE = 1, 5
fps = None


def wait(seconds):
    """A `wait` line of about that many seconds, at the copy's frame rate, measured once."""
    global fps
    if fps is None:
        fps = frame_rate()
    return f"wait {max(1, round(seconds * fps))}"


def folder():
    return fx.link() / "scout"


def shot_list():
    path = folder() / "shots.json"
    return json.loads(path.read_text()) if path.exists() else {}


def seconds(clock):
    hours, minutes = clock.split(":")
    return int(hours) * 3600 + int(minutes) * 60


def aim(eye, target):
    """The yaw and pitch that look from `eye` at `target`."""
    dx, dy, dz = (t - e for t, e in zip(target, eye))
    return [math.atan2(dx, dz), math.atan2(dy, math.hypot(dx, dz))]


def compare(on, off, name):
    """<name>-split.png: vanilla's left half next to Phantom FX's right half, a thin line between.
    <name>-ab.png: both whole frames at half size, vanilla on the left."""
    left, right = np.asarray(Image.open(off)), np.asarray(Image.open(on))
    middle = left.shape[1] // 2
    picture = np.concatenate([left[:, :middle], right[:, middle:]], 1)
    picture[:, middle - 1 : middle + 1] = 255
    Image.fromarray(picture).save(folder() / f"{name}-split.png")
    halves = [Image.fromarray(frame).resize((frame.shape[1] // 2, frame.shape[0] // 2)) for frame in (left, right)]
    Image.fromarray(np.concatenate([np.asarray(half) for half in halves], 1)).save(folder() / f"{name}-ab.png")


def failed(result):
    """Whether a line failed: the game starts its answer with "error". The lines themselves are echoed too, and
    may hold the word."""
    return any(line.startswith("error") for line in result.splitlines())


def take(name, shot):
    lines = sky_lines(shot)
    camera = " ".join(str(value) for value in shot["camera"])
    lines += [f"camera {camera}", wait(AFTER_JUMP), "wait_scene", wait(1)]
    lines += [f"image scout/{name}-on", "enhancements 0", wait(VANILLA), f"image scout/{name}-off", "enhancements 1", "release"]
    result = fx.run(lines)
    if failed(result):
        sys.exit(result)

    on = fx.png(str(folder() / f"{name}-on.dds"))
    off = fx.png(str(folder() / f"{name}-off.dds"))
    compare(on, off, name)
    for picture in ("on", "off"):
        (folder() / f"{name}-{picture}.dds").unlink()

    shots = shot_list()
    shots[name] = {"map": fx.run(["lua vars.missionCode"]).splitlines()[1], **shot}
    (folder() / "shots.json").write_text(json.dumps(shots, indent=2))
    print(f"{folder() / name}-on.png\n{folder() / name}-ab.png")


# Soldiers would find and kill the player between shots, and walk through them: every one on the map is turned off.
CALM = (
    "lua for _, soldiers in pairs(mvars.ene_soldierDefine or {}) do for _, name in ipairs(soldiers) do "
    "local id = GameObject.GetGameObjectId(name) "
    "if id and id ~= GameObject.NULL_ID then GameObject.SendCommand(id, {id = 'SetEnabled', enabled = false}) end end end"
)


def sky_lines(found):
    lines = ["release", "wait 5", CALM]
    if "player" in found:
        x, y, z = found["player"]
        lines += [f"lua TppPlayer.Warp{{pos={{{x}, {y}, {z}}}, rotY=0}}", "wait 300"]
    # A dead player's game-over screen would fill every picture.
    lines.append("lua if vars.playerLife <= 0 then error('the player is dead: restart the copy') end")
    return lines + [f"clock {seconds(found.get('clock', '12:00'))}", f"weather {WEATHERS.index(found.get('weather', 'sunny'))}", wait(SETTLE)]


# How far above the highest surface near it a surveyed camera stays, and how near counts.
CLEARANCE, NEAR = 4.0, 6.0
# The top-down view the heights come from, through a 21 mm lens: it sees 0.29 x its height to each side of the
# place along z (measured, the narrow way), and is at least this high.
LEAST_ALTITUDE, SIDE_PER_ALTITUDE = 300, 0.29


class Heights:
    """The top of whatever the game draws around a place, from straight above: terrain, roofs and trees. One record
    of every pixel (`inspect`) gives each pixel's distance to the game's surface; with the frame's inverse
    view-projection those become world positions."""

    def __init__(self, centre, radius, extra=()):
        """`extra` lines run on the same top-down frame before it is let go."""
        import case

        self.eye = [centre[0], centre[1] + max(LEAST_ALTITUDE, (radius + 2 * NEAR) / SIDE_PER_ALTITUDE), centre[2]]
        eye = self.eye
        result = fx.run(["release", f"camera {eye[0]} {eye[1]} {eye[2]} 0 -1.55 21", wait(AFTER_JUMP), "wait_scene", "inspect scout/heights", *extra, "release"])
        if failed(result):
            sys.exit(result)

        record = case.Record(folder() / "heights")
        distance = record.pixels["game_distance"]
        # As DepthPosition: the pixel's centre at depth ~0 in clip space, through the inverse as stored, rows first.
        inverse = np.asarray(record.header["inverse"], np.float64).reshape(4, 4)
        ys, xs = np.mgrid[0 : record.height, 0 : record.width]
        clip = np.stack([2 * (xs + 0.5) / record.width - 1, 1 - 2 * (ys + 0.5) / record.height, np.full(xs.shape, 1e-6), np.ones(xs.shape)], -1)
        far = clip @ inverse.T
        far = far[..., :3] / far[..., 3:]
        origin = np.asarray(record.header["eye"][:3], np.float64)
        direction = far - origin
        direction /= np.linalg.norm(direction, axis=-1, keepdims=True)
        drawn = distance > 0
        self.world = np.where(drawn[..., None], origin + direction * distance[..., None], np.nan)
        self.points = self.world[drawn]
        for suffix in (".bin", ".tsv"):
            (folder() / f"heights{suffix}").unlink(missing_ok=True)

    def top(self, x, z):
        """The highest surface within NEAR metres of (x, z), or None outside the view."""
        near = (np.abs(self.points[:, 0] - x) < NEAR) & (np.abs(self.points[:, 2] - z) < NEAR)
        return float(self.points[near, 1].max()) if near.any() else None

    def above(self, position, clearance=CLEARANCE):
        """`position` raised to `clearance` over the surface there; outside the view, refused."""
        top = self.top(position[0], position[2])
        if top is None:
            sys.exit(f"no heights at {position[0]:.0f}, {position[2]:.0f}: outside the top-down view")

        return [position[0], max(position[1], top + clearance), position[2]]


# The grid on a map, in metres.
GRID = 50


def top_map(name, centre, radius):
    """A top-down picture of a place with the world's x and z every GRID metres drawn on it, from the same frame's
    record of every pixel: what is where, to aim shots at (water, roofs, open ground)."""
    heights = Heights(centre, radius, [f"image scout/{name}-map"])
    picture = view(folder() / f"{name}-map.dds")
    world = heights.world
    if picture.size != world.shape[1::-1]:
        picture = picture.resize(world.shape[1::-1])
    pixels = np.asarray(picture).copy()
    for axis, colour in ((0, (255, 255, 0)), (2, (0, 255, 255))):
        cell = np.floor(world[..., axis] / GRID)
        edge = np.zeros(cell.shape, bool)
        edge[:, 1:] |= cell[:, 1:] != cell[:, :-1]
        edge[1:, :] |= cell[1:, :] != cell[:-1, :]
        pixels[edge & ~np.isnan(cell)] = colour
    picture = Image.fromarray(pixels)
    draw = ImageDraw.Draw(picture)
    height, width = world.shape[:2]
    for y in range(0, height, max(1, height // 8)):
        for x in range(0, width, max(1, width // 8)):
            if not np.isnan(world[y, x, 0]):
                draw.text((x + 3, y + 3), f"{world[y, x, 0]:.0f},{world[y, x, 2]:.0f} y{world[y, x, 1]:.0f}", fill=(255, 255, 255))
    picture.save(folder() / f"{name}-map.png")
    print(f"x lines yellow, z lines cyan, every {GRID} m; labels are x,z and the surface's height")
    print(folder() / f"{name}-map.png")


def lens(found):
    return float(found.get("mm", LENS))


def survey(name, target, found):
    radius, height, count = float(found.get("radius", 120)), float(found.get("height", 40)), int(found.get("count", 8))
    outward = "outward" in found
    clearance = float(found.get("clearance", CLEARANCE))
    heights = Heights(target, 0 if outward else radius)
    poses = []
    for index in range(count):
        angle = 2 * math.pi * index / count
        if outward:
            poses.append([round(value, 4) for value in heights.above(target, clearance) + [angle, float(found["outward"])]] + [lens(found)])
            continue

        # Never under what the game draws: the heights add the clearance over terrain, roofs and trees.
        eye = heights.above([target[0] - radius * math.sin(angle), target[1] + height, target[2] - radius * math.cos(angle)], clearance)
        poses.append([round(value, 4) for value in eye + aim(eye, target)] + [lens(found)])

    lines = sky_lines(found)
    for index, pose in enumerate(poses):
        lines += [f"camera {' '.join(map(str, pose))}", wait(AFTER_JUMP)] + pictures(name, index, found)
    sheet(name, "survey", lines, [" ".join(map(str, pose)) for pose in poses], "ab" in found)


def times(name, pose, found):
    clocks = found["clocks"].split(",")
    lines = sky_lines(found)
    for index, clock in enumerate(clocks):
        lines += ["release", f"clock {seconds(clock)}", wait(1), f"camera {' '.join(map(str, pose))}", wait(AFTER_JUMP)] + pictures(name, index, found)
    sheet(name, "times", lines, clocks, "ab" in found)


def frame_rate():
    """Presents a second, as the copy runs now: its window out of sight renders at its own pace."""
    began = time.monotonic()
    fx.run(["wait 120"])
    return 120 / (time.monotonic() - began)


def timelapse(name, shot, start, end, seconds_long):
    """The game runs under a held camera with its clock sped up (TppCommand.Weather.SetClockTimeScale, 20 as
    the game has it), as a recording would see it; about six pictures a second go into a preview."""
    span = (seconds(end) - seconds(start)) % 86400
    step = max(1, round(frame_rate() / 6))
    count = int(seconds_long * 6)
    lines = sky_lines(shot | {"clock": start}) + ["live 1", f"camera {' '.join(map(str, shot['camera']))}", wait(AFTER_JUMP), "wait_scene"]
    lines += [f"clock {seconds(start)} run", f"lua TppCommand.Weather.SetClockTimeScale({span / seconds_long:.1f})"]
    for index in range(count):
        lines += [f"wait {step}", f"image scout/{name}-tl{index}"]
    lines += ["lua TppCommand.Weather.SetClockTimeScale(20)", f"clock {seconds(end)}", "release"]
    result = fx.run(lines, timeout=1800)
    if failed(result):
        sys.exit(result)

    views = []
    for index in range(count):
        dds = folder() / f"{name}-tl{index}.dds"
        views.append(Image.open(fx.png(str(dds))).resize((960, 540)))
        dds.unlink()
        dds.with_suffix(".png").unlink()
    out = folder() / f"{name}-timelapse.webp"
    views[0].save(out, save_all=True, append_images=views[1:], duration=1000 // 6, loop=0, quality=80)
    print(out)


def pictures(name, index, found):
    """Lines saving a view as <name>-<index>, and with --ab also as vanilla, <name>-<index>v, once the lighting is back."""
    lines = [f"image scout/{name}-{index}"]
    if "ab" in found:
        lines += ["enhancements 0", wait(VANILLA), f"image scout/{name}-{index}v", "enhancements 1", wait(WARM_UP)]
    return lines


def view(path):
    """A saved view as a picture, the DDS and its PNG removed."""
    picture = Image.open(fx.png(str(path))).convert("RGB")
    path.unlink()
    path.with_suffix(".png").unlink()
    return picture


def sheet(name, kind, lines, labels, isAb=False):
    """Runs `lines`, which save the views <name>-0 to -n, and puts them on one labelled contact sheet. With vanilla
    views too, each is shown beside Phantom FX's with how far apart they are: the mean change of a pixel, in percent."""
    result = fx.run(lines + ["release"])
    if failed(result):
        sys.exit(result)

    columns = 2 if isAb else 4
    tile = 960 if isAb else 480
    picture = Image.new("RGB", (tile * columns, 270 * math.ceil(len(labels) / columns)))
    scores = []
    for index, label in enumerate(labels):
        phantom = view(folder() / f"{name}-{index}.dds")
        small = phantom.resize((480, 270))
        if isAb:
            vanilla = view(folder() / f"{name}-{index}v.dds")
            change = np.abs(np.asarray(phantom, np.float32) - np.asarray(vanilla, np.float32)).mean() / 2.55
            scores.append((change, index, label))
            label = f"{label}  change {change:.1f}%"
            pair = Image.new("RGB", (960, 270))
            pair.paste(vanilla.resize((480, 270)), (0, 0))
            pair.paste(small, (480, 0))
            small = pair
        ImageDraw.Draw(small).text((6, 4), f"{index}: {label}", fill=(255, 255, 0))
        picture.paste(small, (tile * (index % columns), 270 * (index // columns)))
        print(f"{index}: {label}")
    for change, index, label in sorted(scores, reverse=True)[:3]:
        print(f"most changed: {index} ({change:.1f}%)")
    picture.save(folder() / f"{name}-{kind}.png")
    print(folder() / f"{name}-{kind}.png")


# The middle of each outpost's soldiers, or where one outpost's soldiers stand: places on the ground.
SPOTS = """
local function where(name)
  local id = GameObject.GetGameObjectId(name)
  if id == nil or id == GameObject.NULL_ID then return nil end
  return GameObject.SendCommand(id, {id = "GetPosition"})
end
local lines = {}
for outpost, soldiers in pairs(mvars.ene_soldierDefine) do
  if type(outpost) == "string" and (OUTPOST == nil and not outpost:find("lrrp") or outpost == OUTPOST) then
    local x, y, z, count = 0, 0, 0, 0
    for _, name in ipairs(soldiers) do
      local p = where(name)
      if p and OUTPOST then table.insert(lines, string.format("%s %.1f %.1f %.1f", name, p:GetX(), p:GetY(), p:GetZ())) end
      if p then x, y, z, count = x + p:GetX(), y + p:GetY(), z + p:GetZ(), count + 1 end
    end
    if count > 0 and not OUTPOST then table.insert(lines, string.format("%s %.0f %.0f %.0f", outpost, x / count, y / count, z / count)) end
  end
end
table.sort(lines)
return table.concat(lines, "/")
"""


def spots(outpost):
    code = f"local OUTPOST = {json.dumps(outpost) if outpost else 'nil'} " + SPOTS.strip().replace("\n", " ")
    print(fx.run([f"lua {code}"]).splitlines()[1].replace("/", "\n"))


def options(arguments):
    found, rest = {}, []
    words = iter(arguments)
    for word in words:
        if word in ("--player", "--look"):
            found[word[2:]] = [float(next(words)) for _ in range(3)]
        elif word.startswith("--"):
            found[word[2:]] = next(words)
        else:
            rest.append(word)

    return found, rest


def main():
    fx.take_copy_flag(["--copy"])
    command, *arguments = sys.argv[1:] or [None]
    found, rest = options(arguments)
    folder().mkdir(exist_ok=True)
    if command in ("shot", "times"):
        name, *camera = rest
        camera = [float(value) for value in camera]
        if "look" in found:
            camera += aim(camera, found["look"])
        camera = [round(value, 4) for value in camera[:5]] + [lens(found)]
    if command == "times":
        times(name, camera, found)
    elif command == "shot":
        shot = {"camera": camera, "clock": found.get("clock", "12:00"), "weather": found.get("weather", "sunny")}
        shot |= {key: found[key] for key in ("player", "note") if key in found}
        take(name, shot)
    elif command == "timelapse":
        shot = shot_list()[rest[0]]
        timelapse(found.get("as", rest[0]), shot, rest[1], rest[2], float(found.get("seconds", 10)))
    elif command == "again":
        shot = shot_list()[rest[0]] | {key: found[key] for key in ("clock", "weather", "note") if key in found}
        shot.pop("map")
        take(found.get("as", rest[0]), shot)
    elif command == "survey":
        survey(rest[0], [float(value) for value in rest[1:4]], found)
    elif command == "restart":
        settings = Path(tempfile.gettempdir()) / "scout-PhantomFX.ini"
        shutil.copy(fx.AUTOMATION_GAME / "PhantomFX.ini", settings)
        fx.restart_copy(settings)
        print(f"in the game after {fx.boot():.1f} s")
    elif command == "map":
        top_map(rest[0], [float(value) for value in rest[1:4]], float(found.get("radius", 150)))
    elif command == "spots":
        spots(rest[0] if rest else None)
    elif command == "list":
        for name, shot in shot_list().items():
            print(f"{name}: {shot['map']} {shot['clock']} {shot['weather']} - {shot.get('note', '')}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
