# Automation

Status: proposal, 2026-11-20; phase 1 built. `docs/DEVELOPMENT.md` ("Debug cases") describes what exists. This says
what to keep, what to replace, and why.

## Verdict: does a dedicated MCP help?

Yes, as a bridge. No, as a wrapper.

An MCP fixes how a model reaches the tools: typed arguments and descriptions the harness shows it, structured
results and errors, one place for session state and safety, resources for reports and pictures, and jobs for work
longer than a tool call. That is most of why weaker models fail today.

An MCP does not fix what the tools can do. One MCP tool per line of `src/devkit/automation.hpp` would make the surface
larger and the descriptions thinner: 28 tools to choose between, with the same narrow verbs underneath. The rebuild
is the tool surface, not the protocol.

Recommendation: 8 capability tools plus resources, `run` kept as the escape hatch, and the existing file protocol
underneath until it measurably hurts. About 7 working days in four phases, each leaving today's tools working.

## What exists now

| Layer | What it is | Files |
| --- | --- | --- |
| Game | 28 line verbs mapped almost one to one onto internal functions; writes one text result per command file | `src/devkit/automation.*`, `src/dev/devkit_link.cpp` |
| Bridge | Four CLIs that write files, poll every 50 ms, and parse text and DDS | `fx.py`, `case.py`, `perf.py`, `scout.py` |
| Agent | A 120-line skill holding commands, paths, safety rules and how to read reports | `.agents/skills/mgsv-automation/` |

`fx.py run` waits up to 600 s for one result file. There is no help, no version, no progress, no cancel and no
per-line status: one failure at line 7 of 12 comes back as prose after everything has run.

## Why it fails

**"Too specific."** Each verb exists because one bug needed it: `inspect`, `drawn`, `owner`, `textures`, `game_shadow`,
`record_costs`, `remaster`. Asking a new question means changing C++, a script and the skill. The agent's
investigation is bounded by the questions someone thought of yesterday. A model cannot compose `probe`, `owner` and
`reveal` into "what is in this region, who owns it, and why was it left out" without many round trips and manual
joins.

**"Not powerful enough."** Queries need exact render pixels or model hashes. There is no region query, no filter over
scene objects, no stable machine-readable report, and no way to ask what this build supports. Fixed workflows live in
scripts (`replay`, `scenarios`) instead of primitives the agent can arrange.

**"Some models fail."** The surface is a shell CLI, so a model must quote correctly, remember `--copy`, know the
timeout, and read prose. A 600 s call exceeds many clients' tool timeout, and without progress a model retries and
duplicates work. The skill plus command table costs context before the first action, and weaker models drop the safety
rules first. Nothing in code enforces those rules: "never touch the player's game" is a sentence.

## Target shape

```
agent (any MCP client)          human (shell)
        |                            |
  MCP tools + resources         thin CLI
        \                           /
          bridge: state, safety, jobs, DDS->PNG, checks
                        |
              .cmd / .result.json files
                        |
          game: automation engine (unchanged role)
```

The game stays the only C++ part. The bridge is one Python package; the CLI and the MCP server are two front ends to
it, so there is one implementation of parsing, paths and safety.

The runner is DevKit's, and it is not PhantomFX's: `DevKit.addon64` owns the command files, the waits, F9, the saves
redirection and the `DevKit` folder (`src/devkit/automation.*`), with the lines that stand on the game alone (`lua`,
`travel`, `cutscene`, `state`, `status`, `peek`, `help`). Any add-on plugs its own lines, state and F9 into it through
`DevKitGetApi` (`src/devkit/extension.hpp`). PhantomFX is one: `src/dev/devkit_link.cpp` registers the camera, the
records, the costs, the probes, the scene, the settings and the sky, and the debug case. DevKit includes nothing of
PhantomFX; without PhantomFX those lines say they are unknown, and `help` lists what is there.

### Tools, v1

| Tool | What it does | Replaces |
| --- | --- | --- |
| session | `status`, `open player\|copy`, `freeze`, `release`, `start` (install, launch, free roam), `stop`, `boot`, `travel`, `acc`, `recover` (a copy stuck on a screen that waits for a key: presses it, no restart), `show` / `hide` (the copy's window on screen to watch or play, or off it again); holds which game and build for later calls | `fx.py status`, `boot`, `acc`, `travel`, `install.ps1`, `launch.ps1` |
| `run` | A batch of command lines, DevKit's and the add-ons', each answered `ok` or an error; the escape hatch for anything not below | `fx.py run` |
| `observe` | One query: `pixel`, `region`, `object`, `excluded`, `memory`, `lua`; filters by hash, owner, bounds or reason left out; returns rows and paths | `probe`, `peek`, `drawn`, `owner`, `lua` |
| `capture` | `image`, `inspect` or `perf` on the frozen frame; returns files plus a preview as image content | `image`, `inspect`, `textures`, `perf`, `png`, `diff` |
| `case` | `latest`, `open`, `record`, `replay`, `compare`; replay is a job | `case.py`, `fx.py replay` |
| `verify` | Runs scenarios, returns per-check deltas against the limits and a pass or fail per check | `fx.py scenarios` |
| `setting` | Reads or writes a `PhantomFX.ini` key, with a restore token; a player-game write needs `confirm` | `setting`, ini edits |
| `weather` | The sky: `get`, `set` a type (0 sunny, 1 cloudy, 2 rainy, 3 sandstorm, 4 foggy, with its fog) or `off`; PhantomFX's WeatherManager | `weather [<type> [<fog>]|off]` |
| `time_of_day` | The clock: reads it, or sets seconds since midnight and stops it unless `run` | `clock [<seconds> [run]]` |
| `job` | `status`, `wait`, `cancel` for replay, scenarios, perf and launch | timeouts and retries |

Travel, clock, weather, live, move, reveal, diagnostic, enhancements and remaster stay `run` lines. Making each a tool
is how the surface became too specific.

A loading screen that waits for Space stops the game's own loop, so a command file cannot press it. `boot`, `travel`
and `recover` press a gamepad A from outside the game, every few seconds while it is not playing: DevKit hooks
XInput and merges the button into what the game reads, so no window is brought forward and the user's focus is not
taken. `recover` is the tool for a copy already sitting on one, and it never restarts the game. When the loop does run,
`key <name> [times]` presses one from a command file. Do not restart a game stuck on such a screen, and do not
improvise a keypress.

### Resources

`case://latest/report.md`, `case://latest/checks.json`, the case pictures, `scenario://list`, `scenario://<name>`,
`perf://latest`. A model reads what it needs when it needs it instead of carrying the skill's whole reference.

### Errors and jobs

Every failure returns `{ok: false, code, message, fix}` with codes such as `NO_DEV_BUILD`, `PLAYER_GAME_RUNNING`,
`COPY_RUNNING`, `FROZEN_IN_LOAD`, `UNKNOWN_LINE`, `NEEDS_CONFIRM`, `TIMEOUT`. `UNKNOWN_LINE` carries the build's own
verb list. Replay, scenarios and launch return `{job, eta_s}` at once; `job` polls, waits or cancels. No tool call
should need to outlive 30 s.

## Game-side changes, minimum

1. `<name>.result.json` beside `<name>.result`: one entry per line with `ok`, `data` or `error`, and milliseconds.
   Keep the text result for humans and the current CLI.
2. `help [<verb>]` lists this build's verbs and argument shapes; `status` names an automation version.
3. `cancel <id>` stops `wait`, `perf`, `inspect` and `move` at the next frame; long verbs write progress every second.
4. `select` over scene objects: hash, owner, bounds, reason left out, pass visibility. `drawn` and `owner` become
   presets of it once it is proven.

Not now: a named pipe, a binary protocol, or a scripting language in the game. `run` and `lua` already cover the tail.

## Safety in code

- The player's settings are written only by `setting` with `confirm`; `run` refuses a `setting` line without it. The
  old value comes back, so a change can be put back.
- `start`, `replay` and `verify` refuse while `mgsvtpp` answers from anywhere but the copy. The bridge, not the skill,
  decides this.
- The bridge stops only a process whose path is under the automation folder.
- A copy the bridge started is closed when the task calls `session stop`, after ten minutes with no tool call, and
  when the server exits. `PHANTOM_FX_AUTO_CLOSE_SECONDS` changes the idle time; 0 turns it off.
- Outputs go under `DevKit` or `mgsv-automation`; the bridge rejects paths outside them, so game images
  cannot reach the repository.

## The skill after the rebuild

Keep: how to read each check, known false positives, what cannot be reached, prove the cause before changing code,
when to ask the user. Drop: command tables, CLI syntax, paths, timeouts, quoting, which script does what. Target under
40 lines. Mechanics live in tool descriptions the harness shows every model; judgment stays in the skill.

## Migration

| Phase | Work | Estimate |
| --- | --- | --- |
| 0 | Agree this surface: 8 tools, their arguments, the error codes and the result envelope | 1 h |
| 1 | MCP server over today's scripts: `session`, `run`, `case`, `capture`, `verify`, `setting`, `job`; safety checks; a fake game folder for tests. No C++ changes | 1 day |
| 2 | C++ JSON results, `help`, `cancel`, progress. The bridge stops parsing prose | 2 days |
| 3 | `observe` and `select`, resources, shrink the skill, a smoke case per tool | 2 days |
| 4 | CLI becomes a thin client of the bridge; delete duplicated parsing from the four scripts | 1 day |

Phase 1 is the experiment that answers "does this help the models": the same tools, a better surface, no game changes.
Stop there if it does not.

## Phase 1, built

`tools/automation/mcp_server.py` is the server: `session`, `run`, `case`, `capture`, `verify`, `setting`, `job`,
`weather` and `time_of_day`.
It wraps today's scripts, so the game needs no changes. `tools/automation/test_mcp.py` checks the file handshake,
the refusals and the paths without the game. Both were run on 2026-11-20: the tools are listed by an MCP client,
`case` returns a real report and its pictures as image content, and `setting` reads the copy's ini.

Run it from the repository root; an MCP client starts the same command:

```
uv run -q --with mcp --with numpy --with pillow --with scipy python tools/automation/mcp_server.py
```

Register that command in the MCP client. `PHANTOM_FX_GAME` and `PHANTOM_FX_AUTOMATION_GAME` (read in `fx.py`
and the two PowerShell scripts) point the CLI, the server and the copy's installer at other installs. `PHANTOM_FX_DEV_BUILD`
selects another build. `session` with action `start` drives the whole way as one job: install the checkout's
`build/dev` into the copy, launch it, press the startup notices away (autosave, online information), skip the
Konami and Fox logos (`skip-logos`, `automation.cpp`) and step into free roam. It waits 45 s before its first Lua
call, as Infinite Heaven's hooks over the game's Lua can crash a call made earlier. Long work is a job; `case` and `capture` return pictures as image content, not paths alone.

Not yet: `observe` and the game-side JSON results, `help` and cancel (phases 2 and 3).

## What "works with models" means

The measure is not "an MCP exists". It is:

- A model that has never seen this repository goes from F9 to a stated, proven cause and a replayed fix without
  reading `DEVELOPMENT.md`.
- Swapping the model changes nothing about the workflow.
- Every failure names a code and the next action; no model has to guess from prose.
- No call times out; long work is a job.
- A player-game write or a copy beside the running game is refused by code, not by a rule the model might drop.

Test it with two models and the same three tasks, and count calls, errors and tokens. That number is the rebuild's
result.

## Not building

- One tool per command; that is the current problem in a new protocol.
- Remote, cloud, auth or a web UI. This is a local Windows bridge.
- Report analysis in C++. `case.py`'s checks stay where they are; the bridge exposes them as data.
- A named pipe until latency, not reliability, is the complaint.
- A rewrite of `replay` or `scenarios`. They work; they become jobs.

## Open questions

1. Which clients must work: OpenCode only, or Claude Code and others too? This decides packaging (uv script or binary).
2. Which models failed, and at which step: choosing a command, quoting it, waiting, or reading the report? That sets
   the test and the tool descriptions.
