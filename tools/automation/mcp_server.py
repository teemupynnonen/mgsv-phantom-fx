"""The automation tools as an MCP server, for any MCP client (dev builds only).

  uv run -q --with mcp --with numpy --with pillow --with scipy python tools/automation/mcp_server.py

It wraps what `fx.py` and `case.py` already do: sessions name the game to talk to, `run` is the escape hatch,
and long work (start, boot, replay, scenarios) returns a job so no tool call outlives a client's timeout.
Every failure is `{ok: false, code, message, fix}`.

The tool surface and its reasons: docs/AUTOMATION.md.
"""
import atexit
import json
import os
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal

sys.path.insert(0, str(Path(__file__).resolve().parent))

import case as report
import fx
from mcp.server.mcpserver import Image, MCPServer
from mcp.types import CallToolResult, TextContent

VERSION = "0.1"
HERE = Path(__file__).resolve().parent
GAMES = {"player": fx.PLAYER_GAME, "copy": fx.AUTOMATION_GAME}

mcp = MCPServer(
    "phantom-fx-automation",
    version=VERSION,
    instructions="Drives the Phantom FX dev build: session status first. Long work returns a job id; poll it with job. Stop the copy when the task is done; a copy started here also closes after ten minutes with no tool call.",
)

session_target = "player"
fx.game_folder = GAMES[session_target]
LOCK = threading.RLock()
# A copy the tools started closes after this many seconds with no tool call, and when the server exits.
AUTO_CLOSE_SECONDS = float(os.environ.get("PHANTOM_FX_AUTO_CLOSE_SECONDS", "600"))
owned_copy = False
last_activity = time.monotonic()
ACTIVITY_LOCK = threading.Lock()


def note_activity() -> None:
    """Every tool call is a reason to keep a running copy alive; the idle timer starts again here."""
    global last_activity
    with ACTIVITY_LOCK:
        last_activity = time.monotonic()


# ---------------------------------------------------------------- results and failures

def ok(payload: dict | None = None, text: str | None = None, images: tuple[Path, ...] = ()) -> CallToolResult:
    body = {"ok": True, **(payload or {})}
    content = [TextContent(type="text", text=text or json.dumps(body, indent=2, ensure_ascii=False))]
    content += [Image(path=str(path)).to_image_content() for path in images if path.exists()]
    return CallToolResult(content=content, structured_content=body)


def fail(code: str, message: str, fix: str) -> CallToolResult:
    body = {"ok": False, "code": code, "message": message, "fix": fix}
    return CallToolResult(content=[TextContent(type="text", text=json.dumps(body, indent=2, ensure_ascii=False))], structured_content=body, is_error=True)


def resolve(target: str | None) -> tuple[str, Path]:
    """The target and its folder; sets what fx.py talks to. Callers hold LOCK."""
    name = target or session_target
    if name not in GAMES:
        raise ValueError(f"target is player or copy, not {name!r}")
    folder = GAMES[name]
    fx.game_folder = folder
    return name, folder


def running() -> list[Path]:
    """The paths of every running mgsvtpp. A game whose path cannot be read is fx.UNREADABLE (never the copy's).
    When they cannot be asked, the answer is one unreadable game: unknown must not read as "none"."""
    try:
        return fx.running_games()
    except (OSError, subprocess.TimeoutExpired):
        return [fx.UNREADABLE]


def player_running() -> bool:
    """Whether a game that is not the copy runs, or whether that cannot be told: either one refuses a copy."""
    return any(path.parent != GAMES["copy"] for path in running())


def copy_running() -> bool:
    return any(path.parent == GAMES["copy"] for path in running())


def stop_copy() -> bool:
    """Stops the copy, never the player's game; whether one was running."""
    if not copy_running():
        return False

    escaped = str(GAMES["copy"]).replace("'", "''")
    subprocess.run(["powershell", "-NoProfile", "-Command",
        f"Get-Process mgsvtpp -ErrorAction SilentlyContinue | Where-Object {{ $_.Path -eq '{escaped}\\mgsvtpp.exe' }} | Stop-Process -Force"],
        capture_output=True, text=True, timeout=30)
    return True


def owned_path() -> Path:
    """A marker in the copy: the tools started it, so the tools may close it after an idle wait or on exit."""
    return GAMES["copy"] / fx.LINK / "mcp-owned"


def own_copy() -> None:
    global owned_copy
    owned_copy = True
    path = owned_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("mcp\n", encoding="utf-8")


def disown_copy() -> None:
    global owned_copy
    owned_copy = False
    owned_path().unlink(missing_ok=True)


def copy_owned() -> bool:
    """Whether a launch made this copy: the marker fx.restart_copy writes, not a folder that happened to be running."""
    return owned_path().exists()


def auto_close() -> None:
    """The backstop: a copy a launch made is closed after AUTO_CLOSE_SECONDS without a tool call, or when
    the server exits. A job still running keeps it; the task's own session stop should come first."""
    while True:
        time.sleep(30)
        if AUTO_CLOSE_SECONDS <= 0 or not copy_owned():
            continue

        with ACTIVITY_LOCK:
            idle = time.monotonic() - last_activity
        if idle < AUTO_CLOSE_SECONDS:
            continue
        with JOB_LOCK:
            if any(job.state == "running" for job in JOBS.values()):
                continue

        if not copy_running():
            disown_copy()
            continue
        if stop_copy():
            disown_copy()
            print(f"closed the automation copy after {idle:.0f} s idle", file=sys.stderr)


def close_on_exit() -> None:
    if copy_owned():
        stop_copy()
        disown_copy()


def needs_copy() -> CallToolResult | None:
    """The refusal a copy-only job needs while the player's game is open: launch.ps1 would restore their saves."""
    if player_running():
        return fail("PLAYER_GAME_RUNNING", "The player's game is running; the automation copy must not start beside it.",
            "Ask the user to close their game, then run this again.")
    return None


def needs_steam() -> CallToolResult | None:
    """The refusal every launch needs: without Steam the game sits on its SteamAPI error dialog, and no scripts run."""
    if fx.steam_running():
        return None
    return fail("STEAM_NOT_RUNNING", "Steam is not running, so the game cannot initialize SteamAPI.",
        "Start Steam and log in, then run this again.")


def normalise(lines: list[str] | str) -> list[str]:
    if isinstance(lines, str):
        lines = [lines]

    return [line.strip() for item in lines for line in str(item).splitlines() if line.strip()]


def target_label() -> str:
    return next((name for name, folder in GAMES.items() if folder == fx.game_folder), session_target)


# ---------------------------------------------------------------- running lines

def send(lines: list[str], timeout: float) -> CallToolResult:
    """One command file, its text back. Callers hold LOCK and have resolved the target."""
    try:
        text = fx.run(lines, timeout=timeout)
    except TimeoutError:
        return fail("NO_DEV_BUILD", f"No result in {timeout:.0f} s from {fx.game_folder}.",
            "Start a dev build there, or read DevKit/launch.log, then session status.")
    except (OSError, PermissionError, fx.GameGone) as error:
        return fail("NO_DEV_BUILD", f"Could not reach {fx.game_folder}: {error}",
            "Start a dev build there, then session status.")

    return ok({"target": target_label(), "lines": lines, "result": text}, text=text or "(no output)")


# ---------------------------------------------------------------- jobs

@dataclass
class Job:
    id: str
    action: str
    process: subprocess.Popen
    output: list[str] = field(default_factory=list)
    started: float = field(default_factory=time.monotonic)

    @property
    def state(self) -> str:
        if self.process.poll() is None:
            return "running"
        return "done" if self.process.returncode == 0 else "failed"


JOBS: dict[str, Job] = {}
JOB_LOCK = threading.Lock()
MUTATING = {"start", "recover", "boot", "travel", "acc", "replay", "scenarios"}
JOB_ETA = {"start": 150, "recover": 90, "boot": 45, "travel": 40, "acc": 20, "replay": 150, "scenarios": 60}


def pump(job: Job) -> None:
    assert job.process.stdout is not None
    for line in job.process.stdout:
        job.output.append(line.rstrip())
        del job.output[:-400]
    job.process.wait()


def start_job(action: str, target: str, extra: list[str], eta: int) -> CallToolResult:
    with JOB_LOCK:
        busy = next((job for job in JOBS.values() if job.state == "running" and job.action in MUTATING), None)
        if busy:
            return fail("BUSY", f"{busy.action} is still running as job {busy.id}.",
                "Poll it with job, or cancel it, before starting another.")
        job_id = f"{action}-{time.strftime('%H%M%S')}-{len(JOBS) + 1}"
        try:
            process = subprocess.Popen([sys.executable, str(Path(__file__)), "--job", action, "--target", target, *extra],
                cwd=str(HERE), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                bufsize=1, env={**os.environ, "PYTHONUNBUFFERED": "1", "PYTHONIOENCODING": "utf-8"})
        except OSError as error:
            return fail("JOB_FAILED", f"{action} could not start: {error}", "Check that uv and the tools are where the server expects them.")
        job = Job(id=job_id, action=action, process=process)
        JOBS[job_id] = job
    threading.Thread(target=pump, args=(job,), daemon=True).start()

    return ok({"job": job_id, "action": action, "state": "running", "eta_s": eta},
        text=f"{action} started as job {job_id} (about {eta} s). Poll it with job.")


def job_payload(job: Job, lines: int = 60) -> dict:
    return {"job": job.id, "action": job.action, "state": job.state, "seconds": round(time.monotonic() - job.started, 1),
        "output": "\n".join(job.output[-lines:])}


# ---------------------------------------------------------------- session

@mcp.tool()
def session(action: Literal["status", "open", "freeze", "release", "boot", "travel", "acc", "start", "stop", "recover", "show", "hide"] = "status",
            target: Literal["player", "copy"] | None = None,
            destination: Literal["afghanistan", "africa", "motherbase"] | None = None) -> CallToolResult:
    """What the game is doing, and which game the tools talk to. `open` sets the default target; `start` installs
    this build into the automation copy, presses its startup notices away and stops in free roam. A loading screen
    that waits for Space is pressed through by boot and travel themselves, and recover does that for a copy already
    sitting on one, without restarting it. show brings the copy's window on screen to watch or play it, hide puts it
    back off screen. boot, travel, acc, start and recover return a job. Stop the copy when the task is done; a copy started here closes after ten minutes with no
    tool call, and when the server exits."""
    note_activity()
    global session_target, owned_copy
    with LOCK:
        if action == "open":
            if target is None:
                return fail("NEEDS_TARGET", "open needs target='player' or target='copy'.", "Call session with action='open', target='copy'.")
            if not GAMES[target].is_dir():
                return fail("NO_GAME", f"{GAMES[target]} is not there.", "Install the game there, or open the other target.")
            session_target = target
            fx.game_folder = GAMES[target]
            return ok({"target": target, "folder": str(GAMES[target]), "running": [str(path) for path in running()]})

        if action in ("show", "hide"):
            if target == "player":
                return fail("WRONG_TARGET", "show and hide move the automation copy's window, never the player's game.",
                    "Ask the user to move their own window.")
            if not any(path.parent == GAMES["copy"] for path in running()):
                return fail("NOT_RUNNING", "No copy is running.", "Start it with session action='start', target='copy'.")
            fx.game_folder = GAMES["copy"]  # The session's default may be the player's folder.
            fx.view(action == "show")
            return ok({"target": "copy", "visible": action == "show"},
                text="The copy is on screen." if action == "show" else "The copy is off screen again.")

        if action == "stop":
            if target == "player":
                return fail("WRONG_TARGET", "The tools never stop the player's game.", "Ask the user to close it; only the copy can be stopped here.")
            stopped = stop_copy()
            disown_copy()
            return ok({"stopped": stopped}, text="Stopped the automation copy." if stopped else "No copy is running.")

        if action == "recover":
            # A copy stuck on a screen that waits for a key: press it, without restarting the game.
            if target == "player":
                return fail("WRONG_TARGET", "recover presses a key in the automation copy; it never starts the player's game.",
                    "Ask the user to close and reopen their game.")
            return start_job("recover", "copy", [], JOB_ETA["recover"])

        if action == "start":
            if target == "player":
                return fail("WRONG_TARGET", "start installs into the automation copy, never the player's game.", "Use session with action='start', target='copy'.")
            refused = needs_steam()
            if refused:
                return refused
            refused = needs_copy()
            if refused:
                return refused
            ini = GAMES["player"] / "PhantomFX.ini"
            if not ini.exists() and (GAMES["copy"] / "PhantomFX.ini").exists():
                ini = GAMES["copy"] / "PhantomFX.ini"  # Only the copy is installed here: keep its settings
            if not ini.exists():
                return fail("NO_INI", "Neither game has a PhantomFX.ini, so there are no settings to copy.", "Start a game once so it writes its settings.")
            result = start_job(action, "copy", [str(ini)], JOB_ETA[action])
            if not result.is_error:
                session_target = "copy"
                fx.game_folder = GAMES["copy"]
            return result

        try:
            name, folder = resolve(target)
        except ValueError as error:
            return fail("BAD_TARGET", str(error), "Use target='player' or target='copy'.")

        if action == "status":
            addon = folder / "PhantomFX.addon64"
            paths = running()
            payload = {"target": name, "folder": str(folder), "exists": folder.is_dir(), "session": session_target,
                "running": {other: [str(path) for path in paths if path.parent == GAMES[other]] for other in GAMES},
                "addon": {"path": str(addon), "seconds_old": round(time.time() - addon.stat().st_mtime, 0)} if addon.exists() else None}
            try:
                payload |= {"responds": True, "game": fx.run(["status"], timeout=5).strip()}
            except (TimeoutError, OSError, PermissionError, fx.GameGone) as error:
                payload |= {"responds": False, "hint": f"No dev build answered in {folder}: {error}"}
            return ok(payload)

        if action in ("freeze", "release"):
            return send([action], timeout=20)

        if action in ("boot", "travel", "acc"):
            if action == "travel" and destination is None:
                return fail("NEEDS_DESTINATION", "travel needs a destination.", "Use destination='afghanistan', 'africa' or 'motherbase'.")
            if not any(path.parent == GAMES[name] for path in running()):
                return fail("NO_DEV_BUILD", f"No {name} game is running.", "Start it, then run boot, travel or acc, or check launch.log.")
            extra = [destination] if destination else []
            return start_job(action, name, extra, JOB_ETA[action])

        return fail("BAD_ACTION", f"Unknown action {action!r}.", "Use status, open, freeze, release, boot, travel, acc, start, stop, recover, show or hide.")


# ---------------------------------------------------------------- run

@mcp.tool()
def run(lines: list[str] | str, timeout_s: float = 120.0, target: Literal["player", "copy"] | None = None, confirm: bool = False) -> CallToolResult:
    """Runs automation lines as one command file and returns what each printed. The escape hatch for anything the
    other tools do not cover; one string per line (the `help` line lists them). `key space` presses Space at
    the game's window, but only while the game's loop runs: a stalled loading screen stops it, and `session action='recover'`
    presses from outside. confirm=true allows a setting line to write the player's ini."""
    note_activity()
    clean = normalise(lines)
    if not clean:
        return fail("EMPTY", "No lines to run.", "Pass at least one automation line.")

    with LOCK:
        try:
            name, _ = resolve(target)
        except ValueError as error:
            return fail("BAD_TARGET", str(error), "Use target='player' or target='copy'.")

        if name == "player" and any(line.split(maxsplit=1)[0] == "setting" for line in clean) and not confirm:
            return fail("NEEDS_CONFIRM", "A setting line writes the player's PhantomFX.ini.",
                "Pass confirm=true when the user asked for this change, or run it against the copy.")

        return send(clean, timeout=max(1.0, min(float(timeout_s), 300.0)))


# ---------------------------------------------------------------- case

def case_folder(name: str | None, target: str) -> Path | None:
    cases = GAMES[target] / fx.LINK / "cases"
    if name:
        found = (cases / name).resolve()
        return found if found.is_relative_to(cases.resolve()) and found.is_dir() else None
    if not cases.is_dir():
        return None
    folders = [folder for folder in cases.iterdir() if (folder / "state.txt").exists()]
    return max(folders, key=lambda folder: folder.stat().st_mtime) if folders else None


def bad_case_name(name: str) -> bool:
    # One word: no separator, no rooted path, no way up, no whitespace.
    path = Path(name)
    return path.name != name or path.is_absolute() or ".." in path.parts or any(character.isspace() for character in name)


def case_payload(folder: Path) -> tuple[dict, tuple[Path, ...]]:
    with LOCK:
        text = report.write(folder) if not (folder / "report.md").exists() else (folder / "report.md").read_text(encoding="utf-8")
    pictures = tuple(folder / name for name in ("scene-check.png", "sun-check.png", "light-ratio.png", "traced.png", "vanilla.png", "before-after.png"))
    numbers = folder / "numbers.json"
    game = next((name for name, root in GAMES.items() if folder.is_relative_to(root)), "player")
    payload = {"game": game, "case": folder.name, "folder": str(folder), "report": text, "pictures": [str(path) for path in pictures if path.exists()],
        "numbers": json.loads(numbers.read_text()) if numbers.exists() else None}
    return payload, pictures


def read_case(folder: Path) -> CallToolResult:
    try:
        payload, pictures = case_payload(folder)
    except (OSError, KeyError, ValueError, IndexError) as error:
        log = folder / "log.txt"
        return fail("CASE_UNREADABLE", f"{folder.name}: {error}", f"Read {log}." if log.exists() else "Record the case again.")
    return ok(payload, images=pictures)


@mcp.tool()
def case(action: Literal["latest", "open", "record", "replay", "compare"] = "latest", name: str | None = None, other: str | None = None,
         target: Literal["player", "copy"] | None = None) -> CallToolResult:
    """A debug case, as F9 records one: its report, checks and pictures. `record` takes a case from the frozen frame;
    `replay` rebuilds and returns one as a job; `compare` puts two cases' numbers side by side."""
    note_activity()
    global session_target, owned_copy
    with LOCK:
        try:
            game, _ = resolve(target)
        except ValueError as error:
            return fail("BAD_TARGET", str(error), "Use target='player' or target='copy'.")
        # A case is one name under cases/: never a path that could reach outside it.
        if name and bad_case_name(name):
            return fail("BAD_NAME", "A case name is one word under the cases folder.", "Use one word of letters, digits, - and _.")
        if other and bad_case_name(other):
            return fail("BAD_NAME", "A case name is one word under the cases folder.", "Use one word of letters, digits, - and _.")

        if action == "replay":
            refused = needs_steam()
            if refused:
                return refused
            refused = needs_copy()
            if refused:
                return refused
            result = start_job("replay", "copy", [name] if name else [], JOB_ETA["replay"])
            if not result.is_error:
                session_target = "copy"
                fx.game_folder = GAMES["copy"]
            return result

        if action == "record":
            name = name or time.strftime("%Y-%m-%d_%H-%M-%S")
            if bad_case_name(name):
                return fail("BAD_NAME", f"{name!r} is not a case name.", "Use one word of letters, digits, - and _.")
            result = send([f"case {name}"], timeout=180)
            if result.is_error:
                return result
            folder = case_folder(name, game)
            if folder is None:
                return fail("CASE_MISSING", f"The game did not write case {name} in {GAMES[game]}.",
                    "Check that the game is frozen on a frame that renders, then read the case's log.txt.")
            return read_case(folder)

        if action == "compare":
            if not name or not other:
                return fail("NEEDS_CASES", "compare needs two case names.", "Pass name=<before> and other=<after>.")
            if bad_case_name(name) or bad_case_name(other):
                return fail("BAD_NAME", "A case name is one word under the cases folder.", "Use one word of letters, digits, - and _.")
            first, second = case_folder(name, game), case_folder(other, game)
            if first is None or second is None:
                return fail("CASE_MISSING", f"No case {name if first is None else other} in {GAMES[game]}.", "Run case with action='latest' to see what is there.")
            try:
                view = report.same_view(first, second)
                payload = {"before": name, "after": other, "same_view": None if view is None else {"surfaces_percent": round(view[0], 2), "eye_metres": round(view[1], 3)}}
                first_numbers, second_numbers = first / "numbers.json", second / "numbers.json"
                if first_numbers.exists() and second_numbers.exists():
                    payload["worse"] = report.worse(json.loads(first_numbers.read_text()), json.loads(second_numbers.read_text()))
            except (OSError, KeyError, ValueError, IndexError) as error:
                return fail("CASE_UNREADABLE", f"{name} and {other}: {error}", "Read each case's log.txt.")
            return ok(payload)

        folder = case_folder(name, game)
        if folder is None and not name and action == "latest":
            folder = case_folder(None, "copy" if game == "player" else "player")
        if folder is None:
            if name and bad_case_name(name):
                return fail("BAD_NAME", "A case name is one word under the cases folder.", "Use one word of letters, digits, - and _.")
            message = f"No case {name} in {GAMES[game]}." if name else f"No case has been recorded in {GAMES[game]}."
            return fail("CASE_MISSING", message, "Press F9 in the game, or ask the user to.")
        return read_case(folder)


# ---------------------------------------------------------------- weather and time

def state_values() -> dict[str, str]:
    """The `state` line's key=value pairs, as the running game reports them."""
    try:
        text = fx.run(["state"], timeout=10)
    except (TimeoutError, OSError, PermissionError, fx.GameGone):
        return {}

    values = {}
    for line in text.splitlines():
        if "=" in line and not line.startswith(">"):
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip()
    return values


@mcp.tool()
def weather(action: Literal["get", "set", "off"] = "get", weather_type: int | None = None, fog: float | None = None,
            target: Literal["player", "copy"] | None = None) -> CallToolResult:
    """The game's sky. get reads the clock and the weather; set forces a type (0 sunny, 1 cloudy, 2 rainy, 3 sandstorm,
    4 foggy, fog's density only for 4); off lets the game's missions choose again. Needs PhantomFX loaded."""
    note_activity()
    with LOCK:
        try:
            resolve(target)
        except ValueError as error:
            return fail("BAD_TARGET", str(error), "Use target='player' or target='copy'.")

        if action == "set":
            if weather_type is None or not 0 <= weather_type <= 4:
                return fail("NEEDS_TYPE", "set needs a weather type.", "0 sunny, 1 cloudy, 2 rainy, 3 sandstorm, 4 foggy.")
            line = f"weather {weather_type}" + (f" {fog}" if fog is not None else "")
            result = send([line], timeout=20)
            if result.is_error:
                return result
        elif action == "off":
            result = send(["weather off"], timeout=20)
            if result.is_error:
                return result

        values = state_values()
        if "weather" not in values:
            return fail("NOT_LOADED", "The game did not answer with its sky.", "Start a dev build with PhantomFX loaded there.")
        requested, _, density = values["weather"].partition(" ")
        return ok({"target": target_label(), "clock": values.get("clock"), "weather": requested, "fog": density or None},
            text=f"weather {requested}" + (f", fog {density}" if density else "") + f"; clock {values.get('clock')}")


@mcp.tool()
def time_of_day(seconds: float | None = None, run: bool = False, target: Literal["player", "copy"] | None = None) -> CallToolResult:
    """The game's clock: without seconds, reads it; with seconds since midnight, sets it there and stops its clock
    unless run=true. Needs PhantomFX loaded."""
    note_activity()
    with LOCK:
        try:
            resolve(target)
        except ValueError as error:
            return fail("BAD_TARGET", str(error), "Use target='player' or target='copy'.")

        if seconds is not None:
            if not 0 <= seconds < 86400:
                return fail("BAD_SECONDS", "The clock is seconds since midnight.", "Pass a number from 0 to 86399.")
            result = send([f"clock {seconds:g}" + (" run" if run else "")], timeout=20)
            if result.is_error:
                return result

        values = state_values()
        if "clock" not in values:
            return fail("NOT_LOADED", "The game did not answer with its clock.", "Start a dev build with PhantomFX loaded there.")
        payload = {"target": target_label(), "clock": values["clock"]}
        if seconds is not None:
            payload["running"] = run
        return ok(payload, text=f"clock {values['clock']}")


# ---------------------------------------------------------------- capture

def capture_path(name: str | None) -> Path:
    link = fx.link()
    rel = Path(name) if name else Path("captures") / (time.strftime("%Y-%m-%d_%H-%M-%S") + f"-{time.time_ns() % 1000:03d}")
    if rel.is_absolute() or ".." in rel.parts:
        raise ValueError("the name must be under DevKit")
    path = link / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


@mcp.tool()
def capture(kind: Literal["image", "inspect", "perf"], name: str | None = None, frames: int = 120,
            target: Literal["player", "copy"] | None = None) -> CallToolResult:
    """Captures the frozen frame: image (a picture back), inspect (a per-pixel record) or perf (what frames cost).
    Paths are under DevKit; the picture is returned as an image, the others as files."""
    note_activity()
    with LOCK:
        try:
            resolve(target)
        except ValueError as error:
            return fail("BAD_TARGET", str(error), "Use target='player' or target='copy'.")

        try:
            path = capture_path(name)
        except ValueError as error:
            return fail("BAD_PATH", str(error), "Use a relative name such as captures/my-shot.")

        rel = path.relative_to(fx.link()).with_suffix("").as_posix()
        if kind == "image":
            dds = path.with_suffix(".dds")
            dds.unlink(missing_ok=True)  # A failed save must not leave the earlier picture looking new
            result = send([f"image {rel}"], timeout=60)
            if result.is_error:
                return result
            if not dds.exists():
                return fail("NO_PICTURE", f"The game wrote no {dds.name}.", "Freeze on a frame that renders, then try again.")
            picture = fx.png(str(dds))
            return ok({"dds": str(dds), "png": picture}, images=(Path(picture),))

        if kind == "inspect":
            result = send([f"inspect {rel}"], timeout=180)
            if result.is_error:
                return result
            files = [path.with_suffix(".bin"), path.with_suffix(".tsv")]
            return ok({"files": [str(file) for file in files if file.exists()]})

        result = send([f"perf {rel} {max(1, min(frames, 3600))}"], timeout=180)
        if result.is_error:
            return result
        numbers = path.with_suffix(".json")
        return ok({"file": str(numbers), "costs": json.loads(numbers.read_text()) if numbers.exists() else None})


# ---------------------------------------------------------------- verify

@mcp.tool()
def verify(names: list[str] | str | None = None, accept: bool = False) -> CallToolResult:
    """Runs the saved scenarios in a fresh copy with the player's settings, and returns a job. It says what got
    worse and writes a before and after picture for each. accept takes the run as the new expectation."""
    note_activity()
    global session_target, owned_copy
    refused = needs_steam()
    if refused:
        return refused
    refused = needs_copy()
    if refused:
        return refused
    if not (fx.PLAYER_GAME / "PhantomFX.ini").exists():
        return fail("NO_INI", "The player's PhantomFX.ini is not there, so scenarios cannot take its settings.",
            "Start the player's game once so it writes its settings.")
    chosen = normalise(names) if names else []
    scenarios = fx.load_scenarios()
    unknown = [name for name in chosen if name not in {scenario["name"] for scenario in scenarios}]
    if unknown:
        return fail("NO_SCENARIO", f"No scenario named {', '.join(unknown)}.", "Run `fx.py scenarios` to list them.")
    count = len(chosen) or len(scenarios)
    result = start_job("scenarios", "copy", [*(["--accept"] if accept else []), *chosen], max(60, 60 * max(1, count)))
    if not result.is_error:
        session_target = "copy"
        fx.game_folder = GAMES["copy"]
    return result


# ---------------------------------------------------------------- setting

@mcp.tool()
def setting(key: str, action: Literal["get", "set"] = "get", value: str | None = None,
            target: Literal["player", "copy"] | None = None, confirm: bool = False) -> CallToolResult:
    """One PhantomFX.ini key, read or written. A write goes through the running game and is saved; the player's ini
    needs confirm=true, and the old value comes back so it can be put back."""
    note_activity()
    if not key or "\n" in key or "=" in key:
        return fail("BAD_KEY", "Give one key, such as ray_tracing.", "Read docs/DEVELOPMENT.md for the settings.")
    if value is not None and "\n" in value:
        return fail("BAD_VALUE", "A value is one line.", "Pass a single-line value.")

    with LOCK:
        try:
            game, folder = resolve(target)
        except ValueError as error:
            return fail("BAD_TARGET", str(error), "Use target='player' or target='copy'.")

        ini = folder / "PhantomFX.ini"
        if not ini.exists():
            return fail("NO_INI", f"{ini} is not there.", "Start that game once so it writes its settings, or use the other target.")
        old = None
        for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
            found = re.match(rf"\s*{re.escape(key)}\s*=\s*(.*)$", line)
            if found:
                old = found.group(1).strip()
                break

        if action == "get":
            if key and old is None:
                return fail("NO_SETTING", f"{key} is not in {ini}.", "Read the file, or use a key the add-on writes.")
            return ok({"key": key, "value": old, "ini": str(ini)})

        if value is None:
            return fail("NEEDS_VALUE", "set needs a value.", "Pass the value to write.")
        if game == "player" and not confirm:
            return fail("NEEDS_CONFIRM", f"This writes the player's {ini}; it is {old!r} now.",
                "Ask the user first, then pass confirm=true. The old value is in this answer.")
        result = send([f"setting {key}={value}"], timeout=30)
        if result.is_error:
            return result
        answer = result.structured_content.get("result", "")
        if "error:" in answer:
            return fail("SETTING_REFUSED", answer.strip(), "Read PhantomFX.ini for the key and the values it takes.")
        return ok({"key": key, "old": old, "new": value, "ini": str(ini)})


# ---------------------------------------------------------------- job

@mcp.tool()
def job(action: Literal["list", "status", "wait", "cancel"] = "list", job_id: str | None = None, timeout_s: float = 20.0) -> CallToolResult:
    """A long action's progress. wait blocks at most timeout_s (and never past a tool call); cancel stops the job
    and says what it had printed so far."""
    note_activity()
    with JOB_LOCK:
        chosen = JOBS.get(job_id) if job_id else next(reversed(JOBS.values()), None) if JOBS else None
        if action == "list":
            return ok({"jobs": [job_payload(found, 5) for found in JOBS.values()]})
        if chosen is None:
            return fail("NO_JOB", f"No job {job_id}." if job_id else "No job has run yet.", "Start one with session or verify.")

        if action == "status":
            return ok(job_payload(chosen))

        if action == "cancel":
            if chosen.state != "running":
                return ok(job_payload(chosen), text=f"{chosen.id} is already {chosen.state}.")
            subprocess.run(["taskkill", "/PID", str(chosen.process.pid), "/T", "/F"], capture_output=True, text=True, timeout=15)
            try:
                chosen.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                chosen.process.kill()
            return ok(job_payload(chosen), text=f"Cancelled {chosen.id}; run start again if it was installing.")

    deadline = time.monotonic() + max(0.0, min(float(timeout_s), 50.0))
    while chosen.state == "running" and time.monotonic() < deadline:
        time.sleep(0.25)
    return ok(job_payload(chosen))


# ---------------------------------------------------------------- job child

def job_main(arguments: list[str]) -> None:
    """The child a job runs: the same fx.py actions, in a process that can be cancelled."""
    action, *rest = arguments
    target = "copy"
    if "--target" in rest:
        at = rest.index("--target")
        target = rest[at + 1]
        del rest[at:at + 2]
    fx.game_folder = GAMES[target]

    try:
        if action == "recover":
            print(f"playing after {fx.unstick():.1f} s")
        elif action == "start":
            fx.restart_copy(Path(rest[0]))
            print("started the automation copy")
            print(f"in the game after {fx.boot():.1f} s")
        elif action == "boot":
            print(f"in the game after {fx.boot():.1f} s")
        elif action == "travel":
            fx.travel(rest[0])
            print(f"in {rest[0]}")
        elif action == "acc":
            fx.cabin()
            print("in the cabin")
        elif action == "replay":
            fx.replay(rest[0] if rest else None)
        elif action == "scenarios":
            names = [name for name in rest if name != "--accept"]
            sys.exit(1 if fx.run_scenarios(names, "--accept" in rest) else 0)
        else:
            sys.exit(f"unknown job {action}")
    except SystemExit as stop:
        print(f"stopped: {stop}", file=sys.stderr)
        raise
    except Exception as error:  # The parent reads this as the job's failure
        print(f"{type(error).__name__}: {error}", file=sys.stderr)
        raise


if __name__ == "__main__":
    if sys.argv[1:2] == ["--job"]:
        job_main(sys.argv[2:])
    else:
        atexit.register(close_on_exit)
        threading.Thread(target=auto_close, daemon=True).start()
        mcp.run()
