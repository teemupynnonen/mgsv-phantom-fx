"""Talks to the add-on's automation (src/automation.hpp) in a running dev build.

  fx.py [--copy] run <line> [<line> ...]   runs the lines as one command file and prints the result
  fx.py [--copy] status                    what the game is doing
  fx.py [--copy] probe <x> <y> [...]       the shadow and lamp probes at each render pixel
  fx.py [--copy] case [<name>]             records a debug case, as F9 does, and reports it (case.py)
  fx.py replay [<case>]                    a fresh automation copy with this build, back at a case's spot: the same
                                           map, place, time, weather, camera and settings, the case recorded again
                                           and compared; the latest case of either game by default
  fx.py scenario <name> "<why>" [<case>]   keeps a case as a scenario: its spot in scenarios.json with why it
                                           matters and its numbers as what to expect; the latest case by default
  fx.py scenarios [--accept] [<name> ...]  a fresh copy with this build through every scenario, or those named: says
                                           what got worse, with a before and after picture of each. --accept takes
                                           the run as what to expect from now on, after a change meant to show
  fx.py [--copy] boot                      steps the automation copy from launch into free roam
  fx.py [--copy] acc                       from free roam into the helicopter's cabin at Mother Base, once it plays
  fx.py [--copy] travel <map>              another free roam, once it plays
  fx.py png <file.dds> [<out.png>]         a texture as a picture
  fx.py diff <a.dds> <b.dds>               how much two captures differ, and a PNG of where

--copy talks to the automation copy of the game (install.ps1, launch.ps1); the default is the player's game.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

PLAYER_GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\MGS_TPP")
AUTOMATION_GAME = Path.home() / "Documents" / "Development" / "mgsv-automation" / "game"
LINK = "PhantomFX-automation"
# What is expected of each scenario, in the repository; their pictures and depth, which stay on this machine.
SCENARIOS = Path(__file__).resolve().parent / "scenarios.json"
BASELINES = AUTOMATION_GAME.parent / "baselines"
# What of a case's state.txt says where it was.
PLACE = ("mission", "location", "cutscene", "player", "camera", "clock", "weather")

# DXGI formats the textures come in, as NumPy reads them.
R16G16B16A16_FLOAT, R10G10B10A2_UNORM, R8G8B8A8_UNORM, R8G8B8A8_UNORM_SRGB = 10, 24, 28, 29
R16G16_FLOAT, R32_FLOAT, R16_FLOAT, R8_UNORM = 34, 41, 54, 61
B8G8R8A8_UNORM, B8G8R8A8_UNORM_SRGB = 87, 91
HALF_TEXTURES = {R16G16B16A16_FLOAT: 4, R16G16_FLOAT: 2, R16_FLOAT: 1}

game_folder = PLAYER_GAME


def link():
    return game_folder / LINK


def cases():
    return link() / "cases"


def run(lines, timeout=600.0):
    name = time.strftime("%Y%m%d-%H%M%S") + f"-{time.perf_counter_ns() % 1000000:06d}"
    partial = link() / f"{name}.partial"
    partial.write_text("\n".join(lines) + "\n")
    partial.rename(link() / f"{name}.cmd")
    result = link() / f"{name}.result"
    deadline = time.monotonic() + timeout
    while True:
        if time.monotonic() > deadline:
            (link() / f"{name}.cmd").unlink(missing_ok=True)  # Not for a game that starts later
            raise TimeoutError(f"no result for {name} in {timeout} s: is a dev build running in {game_folder}?")
        try:
            text = result.read_text()
            result.unlink()
            return text
        except (FileNotFoundError, PermissionError):  # Not there yet, or still being renamed into place
            time.sleep(0.05)


def texture(path):
    """A DDS file as written by saveTexture: its pixels as NumPy has them, and its DXGI format."""
    data = Path(path).read_bytes()
    height, width = struct.unpack_from("<II", data, 12)
    fmt = struct.unpack_from("<I", data, 128)[0]
    pixels = data[148:]
    if fmt in HALF_TEXTURES:
        channels = HALF_TEXTURES[fmt]
        values = np.frombuffer(pixels, np.float16).astype(np.float32)
        return (values if channels == 1 else values.reshape(-1, channels)).reshape(height, width, *([channels] if channels > 1 else [])), fmt
    if fmt == R32_FLOAT:
        return np.frombuffer(pixels, np.float32).reshape(height, width), fmt
    if fmt == R8_UNORM:
        return np.frombuffer(pixels, np.uint8).reshape(height, width), fmt
    if fmt in (R8G8B8A8_UNORM, R8G8B8A8_UNORM_SRGB):
        return np.frombuffer(pixels, np.uint8).reshape(height, width, 4), fmt
    if fmt in (B8G8R8A8_UNORM, B8G8R8A8_UNORM_SRGB):
        return np.frombuffer(pixels, np.uint8).reshape(height, width, 4)[..., [2, 1, 0, 3]], fmt
    if fmt == R10G10B10A2_UNORM:
        words = np.frombuffer(pixels, np.uint32).reshape(height, width)
        return np.stack([(words >> shift) & 1023 for shift in (0, 10, 20)], -1) / 1023.0, fmt

    raise ValueError(f"DXGI format {fmt} is not handled")


def linear_light(path):
    """A saved back buffer as linear light, whatever its format: PQ nits over 203 for HDR10, else sRGB."""
    pixels, fmt = texture(path)
    if fmt == R10G10B10A2_UNORM:
        m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
        power = pixels ** (1 / m2)
        return 10000 / 203 * (np.maximum(power - c1, 0) / (c2 - c3 * power)) ** (1 / m1)
    if fmt == R16G16B16A16_FLOAT:
        return np.maximum(pixels[..., :3], 0)

    encoded = pixels[..., :3] / 255.0
    return np.where(encoded <= 0.04045, encoded / 12.92, ((encoded + 0.055) / 1.055) ** 2.4)


def shown(light):
    """Linear light as a picture, its highlights rolled off so HDR frames stay readable."""
    rolled = light / (1 + light / 4)
    return (np.clip(rolled, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)


def png(path, out=None):
    out = out or str(Path(path).with_suffix(".png"))
    pixels, fmt = texture(path)
    if pixels.ndim == 2:
        picture = (np.clip(pixels, 0, 1) * 255).astype(np.uint8) if pixels.dtype != np.uint8 else pixels
    elif pixels.shape[-1] == 2:
        picture = (np.clip(np.abs(pixels) / 32, 0, 1) * 255).astype(np.uint8)[..., 0]
    else:
        picture = shown(linear_light(path))
    Image.fromarray(picture).save(out)

    return out


def diff(a, b):
    first, second = linear_light(a), linear_light(b)
    difference = np.abs(first - second).max(-1)
    print(f"pixels changed: {(difference > 0).mean() * 100:.3f}%  max: {difference.max():.3f}  mean: {difference.mean():.4f}")
    out = Path(b).with_name(Path(a).stem + "-vs-" + Path(b).stem + ".png")
    Image.fromarray((np.clip(difference * 4, 0, 1) * 255).astype(np.uint8)).save(out)
    print(f"map: {out}")


# One step towards the game from launch, whatever screen it is on: the start-up sequences and the title
# are Lua sequences (init_sequence.lua, title_sequence.lua), so each is moved on the way the game's own
# buttons would. Popups are closed, and the loading tips left as their key would. Sequences that wait for a
# click are skipped before they start, to where their click leads; the Konami login too, as it opens a
# notice that waits for one.
BOOT_STEP = """
local skips = {
  Seq_Demo_ConfirmAutoSave = "Seq_Demo_StartSignIn",
  Seq_Demo_LogInKonamiServer = "Seq_Demo_CheckPatchDlcForInvitation",
  Seq_Demo_ShowDlcAnnouncePopup = "Seq_Demo_CheckPatchDlc",
}
local t = mvars.seq_sequenceTable
for name, nextName in pairs(skips) do
  local skipped = t and t[name]
  if skipped and not skipped.skippedByAutomation then
    skipped.OnEnter = function() TppSequence.SetNextSequence(nextName) end
    skipped.skippedByAutomation = true
  end
end
gvars.waitLoadingTipsEnd = false
local sequence = TppSequence.GetCurrentSequenceName()
if TppUiCommand.IsShowPopup() then
  TppUiCommand.ErasePopup()
elseif mvars.seq_nowWaitingPushEndLoadingTips then
  mvars.seq_nowWaitingPushEndLoadingTips = nil
  TppUI.FinishLoadingTips()
  GkEventTimerManager.Start("Timer_WaitStartingGame", 1)
elseif skips[sequence] then
  TppUiCommand.ErasePopup()
  TppSequence.SetNextSequence(skips[sequence])
elseif sequence == "Seq_Demo_StartHasTitleMission" then
  TppSequence.SetNextSequence("Seq_Game_PushStart")
elseif sequence == "Seq_Game_TitleMenu" then
  t.Seq_Game_TitleMenu.OnSelectContinue()
end
local playing = TppMission.IsFreeMission(vars.missionCode) and sequence ~= nil and sequence ~= "" and sequence ~= "Seq_Mission_Prepare"
return vars.missionCode .. " " .. tostring(sequence) .. (playing and " playing" or "")
""".strip().replace("\n", " ")


def boot(timeout=600.0):
    """Steps the game from wherever it is into free roam; returns the seconds it took."""
    start = time.monotonic()
    last = None
    while time.monotonic() - start < timeout:
        try:
            state = run([f"lua {BOOT_STEP}", "status"], timeout=30).splitlines()
        except (TimeoutError, FileNotFoundError):
            continue  # The game is starting and not reading commands yet

        if state[1] != last:
            print(f"{time.monotonic() - start:6.1f} s  {state[1]} | {state[3]}", flush=True)
            last = state[1]
        if state[1].endswith(" playing"):
            return time.monotonic() - start

        time.sleep(0.5)

    raise TimeoutError(f"not in the game after {timeout} s")


MAPS = {"afghanistan": 30010, "africa": 30020, "motherbase": 30050}


def travel(destination, place=None, timeout=300.0):
    """Leaves for another free roam, at `place` when given, and returns once it plays: after its loading has
    been seen to start. The game refuses to leave while busy (a fade, a save), so the request repeats until
    it goes. Each poll is a boot step, which closes popups and leaves the loading tips."""
    code = MAPS[destination]
    loading_seen = False
    asked = 0.0
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = run([f"lua {BOOT_STEP}"]).splitlines()[1].split()
        playing = state[-1] == "playing"
        if not playing:
            loading_seen = True
        elif loading_seen and state[0] == str(code):
            return
        elif not loading_seen and state[0] != str(code) and time.monotonic() - asked > 5:
            run([f"travel {destination}" + (f" {place}" if place else "")])
            asked = time.monotonic()
        time.sleep(0.5)

    raise TimeoutError(f"not in {destination} after {timeout} s")


# The helicopter's cabin between missions, by map (TppDefine.SYS_MISSION_ID's *_HELI).
CABINS = {40010, 40020, 40050}


def cabin(code=40050, timeout=120.0):
    """Leaves for the cabin, or from it for a free roam, and returns once that plays. The title screen is the
    same scene, but the title moves on by itself: a freeze there can land in a loading screen, where nothing renders."""
    clear = f"lua TppMission.ReserveMissionClear{{missionClearType = TppDefine.MISSION_CLEAR_TYPE.FROM_HELISPACE, nextMissionId = {code}}}"
    state = 'lua vars.missionCode .. " " .. tostring(TppSequence.GetCurrentSequenceName()) .. " " .. tostring(TppMission.CheckMissionState())'
    print(run([clear]), end="")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        lines = run([f"lua {BOOT_STEP}", state]).splitlines()
        if lines[-2:][0] == f"{code} Seq_Game_MainGame true" or lines[-1] == f"{code} Seq_Game_MainGame true":
            return

        time.sleep(0.5)

    raise TimeoutError(f"not in the cabin after {timeout} s")


LOCATIONS = {"afgh": "afghanistan", "mafr": "africa", "mtbs": "motherbase"}


def state_of(folder):
    lines = (folder / "state.txt").read_text().splitlines()
    return dict(line.split("=", 1) for line in lines if "=" in line)


def case_folder(name=None):
    """A case of the player's game or the copy; the latest of both without a name."""
    folders = [game / LINK / "cases" for game in (PLAYER_GAME, AUTOMATION_GAME)]
    if name:
        found = [folder / name for folder in folders if (folder / name).is_dir()]
        if not found:
            raise SystemExit(f"no case {name}")
        return found[0]

    return max((case for folder in folders if folder.is_dir() for case in folder.iterdir() if (case / "state.txt").exists()), key=lambda case: case.stat().st_mtime)


def windows_powershell():
    """Its environment without PowerShell 7's module path, if that is what started this: with it Windows PowerShell
    finds no Get-FileHash, and launch.ps1 stops at its check of the player's files."""
    return {key: value for key, value in os.environ.items() if key.upper() != "PSMODULEPATH"}


def restart_copy(ini):
    """A fresh automation copy on this checkout's build with `ini`'s settings. Never beside the player's game:
    launch.ps1 puts the player's files back as it ends, which would undo that game's saves."""
    command = "(Get-Process mgsvtpp -ErrorAction SilentlyContinue).Path"
    running = subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True, text=True).stdout.split("\n")
    games = [Path(line.strip()) for line in running if line.strip()]
    if any(game.parent != AUTOMATION_GAME for game in games):
        raise SystemExit("The player's game is running: close it first.")
    if games:
        subprocess.run(["powershell", "-NoProfile", "-Command", "Get-Process mgsvtpp | Stop-Process -Force; Start-Sleep -Seconds 4"], check=True)

    scripts = Path(__file__).resolve().parent
    subprocess.run(["powershell", "-NoProfile", "-File", str(scripts / "install.ps1"), "-Ini", str(ini)], check=True, env=windows_powershell())
    # With a console of its own, unseen: Windows PowerShell does not start in a detached process.
    with open(link() / "launch.log", "w") as log:
        subprocess.Popen(["powershell", "-NoProfile", "-File", str(scripts / "launch.ps1")], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
            env=windows_powershell(), creationflags=subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP)


def go_back(state):
    """The running copy to where a case's player stood: its map and place, by way of the free roam a scenario's
    `via` names, where the way in matters. A story mission or a cutscene comes back as that place in the map's
    free roam, without what the mission or cutscene put there."""
    mission = int(state["mission"])
    here = int(run(["lua vars.missionCode"]).splitlines()[1])
    if "via" in state and here != MAPS[state["via"]]:
        if here in CABINS:
            cabin(MAPS["motherbase"])
        travel(state["via"])
        here = MAPS[state["via"]]

    if mission in CABINS:
        if here != mission:
            cabin(mission)
        return

    if here in CABINS:
        cabin(MAPS["motherbase"])
        here = MAPS["motherbase"]

    names = {code: name for name, code in MAPS.items()}
    name = names.get(mission) or LOCATIONS.get(state.get("location"))
    if not name:
        raise SystemExit(f"mission {mission} at {state.get('location')}: no map to go back to")
    if mission not in names or state.get("cutscene") == "true":
        print("A story mission or cutscene: the place comes back, what it put there does not.")

    x, y, z = state["player"].split()
    if here == MAPS[name]:
        print(run([f"lua TppPlayer.Warp{{pos={{{x}, {y}, {z}}}, rotY=0}}"]), end="")
    else:
        travel(name, f"{x} {y} {z}")


def record_at(state, name):
    """The running copy at a case's or a scenario's spot, recorded there as the case `name`."""
    go_back(state)
    weather = state.get("weather", "-1")
    sky = [f"clock {state['clock']}"] if "clock" in state else []
    sky.append("weather off" if weather.startswith("-1") else f"weather {weather}")
    run(sky + ["wait 120", f"camera {state['camera']}", "wait 60", "wait_scene", f"case {name}"])


def replay(name=None):
    """A fresh copy on this build back at a case, the case recorded again from there and compared."""
    global game_folder
    import case as report

    original = case_folder(name)
    game_folder = AUTOMATION_GAME
    restart_copy(original / "PhantomFX.ini")
    print(f"in the game after {boot():.1f} s", flush=True)
    again = f"{original.name}-replay"
    record_at(state_of(original), again)
    print(report.write(cases() / again))
    view = report.same_view(original, cases() / again)
    print(f"Back at the same view: {view[0]:.1f}% of the game's surfaces match, the eye {view[1]:.3f} m away." if view else "No comparison: a record is missing, or the render sizes differ.")


def load_scenarios():
    return json.loads(SCENARIOS.read_text(encoding="utf-8")) if SCENARIOS.exists() else []


def save_scenarios(scenarios):
    SCENARIOS.write_text(json.dumps(scenarios, indent=2) + "\n", encoding="utf-8")


def keep_baseline(name, folder):
    target = BASELINES / name
    target.mkdir(parents=True, exist_ok=True)
    for file in ("scene.bin", "traced.png"):
        shutil.copy2(folder / file, target / file)


def add_scenario(name, why, case_name=None):
    import case as report

    folder = case_folder(case_name)
    report.write(folder)
    state = state_of(folder)
    entry = {"name": name, "why": why, **{key: state[key] for key in PLACE if key in state}, "expect": json.loads((folder / "numbers.json").read_text())}
    scenarios = [scenario for scenario in load_scenarios() if scenario["name"] != name] + [entry]
    save_scenarios(scenarios)
    keep_baseline(name, folder)
    print(f"Scenario {name} from {folder.name}: {entry['expect']}")


def run_scenarios(names, accept):
    """Each scenario in turn in one fresh copy, with the player's settings: what is expected depends on them."""
    global game_folder
    import case as report

    scenarios = load_scenarios()
    chosen = [scenario for scenario in scenarios if not names or scenario["name"] in names]
    run_name = time.strftime("run-%Y-%m-%d_%H-%M-%S")
    game_folder = AUTOMATION_GAME
    restart_copy(PLAYER_GAME / "PhantomFX.ini")
    print(f"in the game after {boot():.1f} s", flush=True)

    lines = [f"# Scenarios {run_name}", ""]
    failures = 0
    for scenario in chosen:
        name = scenario["name"]
        folder = cases() / f"{run_name}-{name}"
        problems = []
        try:
            record_at(scenario, folder.name)
            report.write(folder)
            measured = json.loads((folder / "numbers.json").read_text())
            baseline = BASELINES / name
            view = report.same_view(baseline, folder) if (baseline / "scene.bin").exists() else None
            if view and (view[0] < report.LEAST_SAME_VIEW or view[1] > report.MOST_EYE_METRES):
                problems.append(f"did not reach the view: {view[0]:.0f}% of the surfaces match, the eye {view[1]:.2f} m away")
            if not accept:
                problems += report.worse(scenario["expect"], measured)
            if (baseline / "traced.png").exists():
                report.side_by_side(baseline / "traced.png", folder / "traced.png", folder / "before-after.png", ("expected", "this run"))
            if accept and not problems:
                scenario["expect"] = measured
                keep_baseline(name, folder)
            run(["release"])
        except (OSError, KeyError, ValueError, TimeoutError) as error:
            problems.append(f"did not run: {error}")

        failures += bool(problems)
        result = "accepted" if accept and not problems else "ok" if not problems else "WORSE: " + "; ".join(problems)
        lines += [f"- **{name}**: {result}", f"  {scenario['why']}", f"  {folder}", ""]
        print(f"{name}: {result}", flush=True)

    if accept:
        save_scenarios(scenarios)

    (cases() / f"{run_name}.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"{len(chosen) - failures} of {len(chosen)} fine. Report: {cases() / run_name}.md")
    return failures


def main():
    arguments = sys.argv[1:]
    global game_folder
    if arguments[:1] == ["--copy"]:
        game_folder = AUTOMATION_GAME
        arguments = arguments[1:]

    if not arguments:
        sys.exit(__doc__)

    command, *rest = arguments
    if command == "run":
        print(run(rest), end="")
    elif command == "status":
        print(run(["status"]), end="")
    elif command == "probe":
        pixels = [f"probe {rest[i]} {rest[i + 1]}" for i in range(0, len(rest), 2)]
        print(run(pixels), end="")
    elif command == "case":
        name = rest[0] if rest else time.strftime("%Y-%m-%d_%H-%M-%S")
        print(run([f"case {name}"]), end="")
        import case as report
        print(report.write(cases() / name))
    elif command == "replay":
        replay(rest[0] if rest else None)
    elif command == "scenario":
        add_scenario(*rest)
    elif command == "scenarios":
        names = [name for name in rest if name != "--accept"]
        sys.exit(1 if run_scenarios(names, "--accept" in rest) else 0)
    elif command == "png":
        print(png(*rest))
    elif command == "diff":
        diff(*rest)
    elif command == "travel":
        travel(rest[0])
    elif command == "boot":
        print(f"in the game after {boot():.1f} s")
    elif command == "acc":
        cabin()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
