# Agent guide

How to work in this repository. `docs/DEVELOPMENT.md` has the build, the tests, the sources and the game; read the
part you are about to change.

## Build and test

- Build with the presets: `cmake --preset release`, then `cmake --build --preset release`. Use `dev` for checks in the
  game (diagnostic log, Developer page, DevKit) and `tools` only for another mod's rendering. Never edit `build/`.
- Run `ctest --test-dir build/release` before saying a change works. The tests need neither the game nor a GPU;
  anything that does needs the game and must say so.
- Warnings are errors (`/W4 /WX`). Fix the cause; do not switch one off.

## Code

- One feature per folder in `src/features/`, with its settings and what draws it. A feature's pull request stays in
  its folder; what several features share belongs in `src/passes/` or `src/core/`.
- Match the surrounding code: its naming, layering, error handling and comment style. Take the shortest diff that
  solves the problem; add no dependency, option or abstraction nobody asked for.
- Comments say why, not what. Player-facing text (menu, log) stays in the player's terms.
- Non-trivial logic leaves a test or a check. Fix a bug at its cause, in the shared code, not at each caller.
- Files are UTF-8, LF, four spaces, a final newline (`.editorconfig`).

## Changelog

- Anything a player sees goes under `## Unreleased` in `CHANGELOG.md`: one bullet, a bold lead sentence, what changed
  and why it matters.
- Never add, move or empty a release heading, and never edit a released section; the maintainer cuts releases.

## Safety

- Never change the game's files, saves or configuration. Memory patches only, as the code already does.
- Don't commit build output, logs, captures or local settings. Check `git status` before staging.
- Don't force-push, rewrite history, or remove branches and worktrees you did not create. Ask before anything
  irreversible or player-visible.

## Verifying lighting

- A lighting fault is checked in the running game, not from memory: use the `.agents/skills/mgsv-automation` skill,
  which records a case and reports it, instead of asking for screenshots or numbers.
- State what was verified and how. If it was not built, tested or checked in the game, say so plainly.

## Commits and pull requests

- Commit subjects: `type(area): what changed`, in plain words, one line. Types in use: `feat`, `fix`, `docs`, `build`,
  `perf`, `test`.
- A pull request says what changed for the player and what was verified. Keep it to one change.
