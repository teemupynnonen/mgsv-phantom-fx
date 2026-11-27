"""Self-check for mcp_server.py without the game: a fake folder answers command files.

  uv run -q --with mcp --with numpy --with pillow --with scipy python tools/automation/test_mcp.py

It covers what can break without a dev build: the file handshake, the player-write refusal, path safety and
the errors a model is meant to act on. The game itself is tested by a case and a replay.
"""
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import mcp_server as server


class FakeGame:
    """A folder that answers every .cmd with one `ok:` line, as the game's automation does."""

    def __init__(self, root):
        self.root = Path(root)
        self.link = self.root / "DevKit"
        self.link.mkdir(parents=True)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stop.is_set():
            for file in sorted(self.link.glob("*.cmd")):
                try:
                    lines = file.read_text(encoding="utf-8").splitlines()
                    file.unlink()
                except (FileNotFoundError, PermissionError):  # Still being renamed, or already gone
                    continue
                result = file.with_suffix(".result")
                partial = result.with_name(result.name + ".partial")
                partial.write_text("\n".join(f"ok: {line}" for line in lines) + "\n", encoding="utf-8")
                partial.replace(result)  # As the runner publishes it: whole, under the name fx.py reads
            time.sleep(0.01)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)


def payload(result):
    return result.structured_content


def with_player(root):
    server.GAMES["player"] = Path(root)
    server.fx.game_folder = Path(root)


def test_run_and_refusals():
    with tempfile.TemporaryDirectory() as temp:
        game = FakeGame(temp)
        with_player(temp)
        try:
            result = server.run(lines=["status"], target="player", timeout_s=5)
            assert not result.is_error, result.content[0].text
            assert "ok: status" in payload(result)["result"], payload(result)

            result = server.run(lines="setting a=1\nstatus", target="player", timeout_s=5)
            assert result.is_error and payload(result)["code"] == "NEEDS_CONFIRM", payload(result)

            result = server.run(lines=["setting a=1"], target="player", timeout_s=5, confirm=True)
            assert not result.is_error, payload(result)

            result = server.session(action="start", target="player")
            assert result.is_error and payload(result)["code"] == "WRONG_TARGET", payload(result)

            result = server.capture(kind="image", name="../escape", target="player")
            assert result.is_error and payload(result)["code"] == "BAD_PATH", payload(result)
        finally:
            game.close()


def test_setting_reads_the_ini():
    with tempfile.TemporaryDirectory() as temp:
        (Path(temp) / "PhantomFX.ini").write_text("ray_tracing=1\nlight_radius=0.5\n", encoding="utf-8")
        with_player(temp)
        result = server.setting(action="get", key="ray_tracing", target="player")
        assert not result.is_error, payload(result)
        assert payload(result)["value"] == "1", payload(result)

        result = server.setting(action="get", key="missing", target="player")
        assert result.is_error and payload(result)["code"] == "NO_SETTING", payload(result)


def test_status_says_no_game():
    with tempfile.TemporaryDirectory() as temp:
        with_player(temp)
        original = server.fx.run

        def no_game(lines, timeout):
            raise TimeoutError("no dev build")

        server.fx.run = no_game
        try:
            result = server.session(action="status", target="player")
            assert not result.is_error, payload(result)
            assert payload(result)["responds"] is False, payload(result)
        finally:
            server.fx.run = original


def test_a_gone_game_is_a_failure_not_a_crash():
    with tempfile.TemporaryDirectory() as temp:
        with_player(temp)
        original = server.fx.run

        def gone(lines, timeout=600.0):
            raise server.fx.GameGone("no game runs there")

        server.fx.run = gone
        try:
            result = server.session(action="status", target="player")
            assert not result.is_error and payload(result)["responds"] is False, payload(result)

            result = server.run(lines=["status"], target="player", timeout_s=5)
            assert result.is_error and payload(result)["code"] == "NO_DEV_BUILD", payload(result)

            result = server.weather(target="player")
            assert result.is_error and payload(result)["code"] == "NOT_LOADED", payload(result)
        finally:
            server.fx.run = original


def test_waiting_outlasts_a_game_not_up_yet():
    """Right after a launch the process is not there at the first look: that is not the game gone."""
    with tempfile.TemporaryDirectory() as temp:
        with_player(temp)
        (Path(temp) / "DevKit").mkdir()
        original = server.fx.run, server.fx.game_running
        asked = []

        def late(lines, timeout=600.0):
            asked.append(lines)
            if len(asked) == 1:
                raise server.fx.GameGone("not up yet")
            return "> status\nframe 500, presented 900, running, camera 0 0 0 0 0 21\n"

        server.fx.run, server.fx.game_running = late, lambda: True
        try:
            server.fx.wait_for_scripts(timeout=5, least_seconds=0)
            assert len(asked) == 2, asked
        finally:
            server.fx.run, server.fx.game_running = original


def test_start_needs_steam():
    original = server.fx.steam_running
    server.fx.steam_running = lambda: False
    try:
        result = server.session(action="start", target="copy")
        assert result.is_error and payload(result)["code"] == "STEAM_NOT_RUNNING", payload(result)
    finally:
        server.fx.steam_running = original


def test_stop_refuses_player():
    result = server.session(action="stop", target="player")
    assert result.is_error and payload(result)["code"] == "WRONG_TARGET", payload(result)


def test_job_list_is_empty():
    result = server.job(action="list")
    assert not result.is_error and payload(result)["jobs"] == [], payload(result)


def test_unknown_games_refuse_a_copy():
    """A game whose path cannot be read, or a query that fails, is not "no game": the copy must not launch beside it."""
    original = server.fx.running_games
    try:
        server.fx.running_games = lambda: [server.fx.UNREADABLE]
        assert server.player_running() and not server.copy_running()

        def fails():
            raise OSError("no answer")

        server.fx.running_games = fails
        assert server.player_running()

        server.fx.running_games = lambda: []
        assert not server.player_running()
        server.fx.running_games = lambda: [server.GAMES["copy"] / "mgsvtpp.exe"]
        assert not server.player_running() and server.copy_running()
    finally:
        server.fx.running_games = original


def test_restart_copy_refuses_an_unreadable_game():
    original = server.fx.running_games
    original_steam = server.fx.steam_running
    server.fx.steam_running = lambda: True
    server.fx.running_games = lambda: [server.fx.UNREADABLE]
    try:
        server.fx.restart_copy(Path("none.ini"))
        raise AssertionError("restart_copy went on beside a game it could not read")
    except SystemExit as stop:
        assert "player's game" in str(stop), stop
    finally:
        server.fx.running_games = original
        server.fx.steam_running = original_steam


def test_show_moves_only_the_copy():
    """With the session on the player, show still reaches the copy's folder, never the player's."""
    original = server.fx.view, server.running, server.session_target
    seen = []
    server.fx.view = lambda show: seen.append(server.fx.game_folder) or True
    server.running = lambda: [server.GAMES["copy"] / "mgsvtpp.exe"]
    server.session_target = "player"
    try:
        result = server.session(action="show")
        assert not result.is_error, payload(result)
        assert seen == [server.GAMES["copy"]], seen
    finally:
        server.fx.view, server.running, server.session_target = original


def test_fx_has_what_it_calls():
    for name in ("view", "weather", "time_of_day", "state_field", "running_games"):
        assert callable(getattr(server.fx, name, None)), name


if __name__ == "__main__":
    for check in (test_run_and_refusals, test_setting_reads_the_ini, test_status_says_no_game, test_a_gone_game_is_a_failure_not_a_crash,
                  test_waiting_outlasts_a_game_not_up_yet, test_start_needs_steam, test_stop_refuses_player, test_job_list_is_empty,
                  test_unknown_games_refuse_a_copy, test_restart_copy_refuses_an_unreadable_game, test_show_moves_only_the_copy, test_fx_has_what_it_calls):
        check()
        print(f"{check.__name__}: ok")

    print("mcp_server: all checks passed")
