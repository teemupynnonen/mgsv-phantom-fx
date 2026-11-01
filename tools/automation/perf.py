"""What frames cost, from the add-on's costs on file (src/dev/perf_record.hpp).

  perf.py [--copy] log [<file>]              a play session by stretch of the same place, then its worst seconds;
                                             the newest session by default
  perf.py [--copy] ab [--frames N] [--settle N] [--images] <test> [<test> ...]
                                             what each test changes on the frame as it stands, frozen first if it
                                             is not: base, test, base, test, base, so a drifting GPU shows. A test is
                                             "<lines> | <lines to undo them>", each set split by ";"

--copy reads the automation copy; the default is the player's game. Run from tools/automation, as fx.py is.
"""
import json
import sys
import time

import fx

# Passes Direct3D 12 times by phase: their phases are listed, not them. GiWait is those phases seen from Direct3D 11.
TRACED_PASSES = {"RayTracedScene", "SunShadowRays", "SunShadowDenoiser", "LightShadowRays", "LightShadowFilter", "GiRays", "GiDenoiser", "Reflections", "LampLight", "GiWait"}


def sessions():
    return sorted((fx.link() / "perf").glob("*.jsonl"))


def rows_of(path):
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:  # The line being written
            pass

    return rows


def clock_text(seconds):
    whole = int(seconds)
    return f"{whole // 3600:02}:{whole % 3600 // 60:02}"


def mean(values):
    values = list(values)
    return sum(values) / len(values) if values else 0.0


def costliest(rows, count=7):
    """The phases and the passes Direct3D 11 times, by their mean over the rows, costliest first."""
    totals = {}
    for row in rows:
        for name, value in row.get("phases", {}).items():
            totals[name] = totals.get(name, 0.0) + value
        for name, value in row.get("passes", {}).items():
            if name not in TRACED_PASSES:
                totals[name] = totals.get(name, 0.0) + value

    ranked = sorted(totals.items(), key=lambda item: -item[1])
    return ", ".join(f"{name} {value / len(rows):.2f}" for name, value in ranked[:count])


def stretch_line(rows):
    first, last = rows[0], rows[-1]
    place = f"{first['location']} {first['mission']}" + (" frozen" if first["frozen"] else "")
    render = "x".join(str(side) for side in first["render"])
    lights = mean(row["lights"]["traced"] + row["lights"]["overCap"] for row in rows)
    head = (f"{first['time']}-{last['time']} {len(rows):4} s  {place}  clock {clock_text(first['clock'])}  render {render}  lights {lights:.0f}  "
        f"frame {mean(row['frame'].get('mean', 0) for row in rows):.1f} ms (p99 {max(row['frame'].get('p99', 0) for row in rows):.1f})  "
        f"GPU {mean(row['gpu'].get('mean', 0) for row in rows):.1f} ms")
    return head + "\n    " + costliest(rows)


def log(name=None):
    if not name and not sessions():
        sys.exit("Nothing recorded: tick Record the costs on the Developer page, or run fx.py run \"record_costs 1\".")

    path = fx.link() / "perf" / name if name else sessions()[-1]
    rows = [row for row in rows_of(path) if "frame" in row]
    print(f"{path.name}: {len(rows)} s")
    stretch = []
    for row in rows:
        same = stretch and all(stretch[-1][key] == row[key] for key in ("mission", "location", "frozen", "render"))
        if stretch and not same:
            print(stretch_line(stretch))
            stretch = []
        stretch.append(row)
    if stretch:
        print(stretch_line(stretch))

    print("\nWorst seconds by GPU time:")
    for row in sorted(rows, key=lambda row: -row["gpu"].get("mean", 0))[:5]:
        print(stretch_line([row]))


def parsed(test):
    lines, _, undo = test.partition("|")
    return ([line.strip() for line in lines.split(";") if line.strip()], [line.strip() for line in undo.split(";") if line.strip()])


def safe_name(index, test):
    letters = "".join(letter if letter.isalnum() or letter == "-" else "_" for letter in test)
    return f"{index}-{letters[:40]}"


def read_capture(folder, name):
    return json.loads((fx.link() / folder / f"{name}.json").read_text(encoding="utf-8"))


def values_of(capture):
    """Each thing a capture timed, by name, in milliseconds."""
    values = {"frame": capture["frame"].get("mean", 0.0), "GPU frame": capture["gpu"].get("mean", 0.0), **capture["phases"]}
    values.update({name: value for name, value in capture["passes"].items() if name not in TRACED_PASSES})
    return values


def report(tests, folder, bases, tried):
    lines = []
    base_values = [values_of(base) for base in bases]
    tried_values = [values_of(capture) for capture in tried]
    # The tests' names too: what a test turns on is in no base.
    names = list(dict.fromkeys(name for values in base_values + tried_values for name in values))
    names.sort(key=lambda name: (name not in ("frame", "GPU frame"), -mean(values.get(name, 0.0) for values in base_values)))
    width = max(len(name) for name in names)
    short = [test.split("|")[0].strip()[:14] for test in tests]
    lines.append(f"{'':{width}} {'base':>7} " + " ".join(f"{label:>14}" for label in short))
    for name in names:
        base = mean(values.get(name, 0.0) for values in base_values)
        cells = []
        for i, values in enumerate(tried_values):
            around = mean((base_values[i].get(name, 0.0), base_values[i + 1].get(name, 0.0)))
            cells.append(f"{values.get(name, 0.0) - around:>+14.2f}")
        if base >= 0.02 or any(abs(float(cell)) >= 0.02 for cell in cells):
            lines.append(f"{name:{width}} {base:7.2f} " + " ".join(cells))

    gpus = [values["GPU frame"] for values in base_values]
    lines.append(f"\nDrift of the base over the run: {max(gpus) - min(gpus):.2f} ms of GPU frame ({' '.join(f'{gpu:.2f}' for gpu in gpus)}). A difference smaller than that says nothing.")
    counters = bases[0].get("counters", {})
    if counters:
        lines.append("\nCounters a frame, millions:")
        lines.append(f"{'':{width}} {'base':>7} " + " ".join(f"{label:>14}" for label in short))
        for name, value in counters.items():
            if value or any(capture["counters"].get(name) for capture in tried):
                lines.append(f"{name:{width}} {value / 1e6:7.3f} " + " ".join(f"{capture['counters'].get(name, 0) / 1e6:>14.3f}" for capture in tried))

    text = "\n".join(lines)
    (fx.link() / folder / "report.txt").write_text(text + "\n", encoding="utf-8")
    return text


def ab(arguments):
    frames, settle, images = 120, 90, False
    tests = []
    words = iter(arguments)
    for word in words:
        if word == "--frames":
            frames = int(next(words))
        elif word == "--settle":
            settle = int(next(words))
        elif word == "--images":
            images = True
        else:
            tests.append(word)

    folder = "perf/ab-" + time.strftime("%Y-%m-%d_%H-%M-%S")
    is_frozen = ", frozen, " in fx.run(["status"])
    start = [] if is_frozen else ["freeze", "wait 30"]
    start += [f"wait {settle}", f"perf {folder}/base-0 {frames}"]
    if images:
        start.append(f"image {folder}/base-0")

    # Each step with what puts the game back if it fails: a command file runs on past a failed line, so the steps
    # go one at a time and nothing after a failure runs.
    steps = [(start, [])]
    for i, test in enumerate(tests):
        apply, undo = parsed(test)
        name = safe_name(i, test)
        tried_lines = [*apply, f"wait {settle}", f"perf {folder}/{name} {frames}"]
        if images:
            tried_lines.append(f"image {folder}/{name}")
        steps += [(tried_lines, undo), ([*undo, f"wait {settle}", f"perf {folder}/base-{i + 1} {frames}"], [])]

    letting_go = [] if is_frozen else ["release"]
    for lines, cleanup in steps:
        output = fx.run(lines, timeout=60 + 6.0 * (frames + settle))
        print(output, end="", flush=True)
        if any(line.startswith("error:") for line in output.splitlines()):
            if cleanup or letting_go:
                print(fx.run(cleanup + letting_go), end="")
            sys.exit("A line above failed, so a table would say the change did nothing.")

    if letting_go:
        print(fx.run(letting_go), end="")

    bases = [read_capture(folder, f"base-{i}") for i in range(len(tests) + 1)]
    tried = [read_capture(folder, safe_name(i, test)) for i, test in enumerate(tests)]
    print(report(tests, folder, bases, tried))
    if images:
        for i, test in enumerate(tests):
            print(f"\n{test}:")
            fx.diff(fx.link() / folder / "base-0.dds", fx.link() / folder / f"{safe_name(i, test)}.dds")
    print(f"\n{fx.link() / folder}")


def main():
    arguments = fx.take_copy_flag(sys.argv[1:])
    if not arguments:
        sys.exit(__doc__)

    command, *rest = arguments
    if command == "log":
        log(*rest)
    elif command == "ab":
        ab(rest)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
