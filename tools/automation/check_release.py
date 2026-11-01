"""Fails if the dev tools reached a release build, looking at the built file itself.

  check_release.py [<add-on>]     build/release/PhantomFX.addon64 by default

They hook the system's file calls, patch Steam's interfaces and run what they are told, which security
software rightly flags, so a release build holds none of it (docs/DEVELOPMENT.md, "The diagnostic log").
Run it on a dev build to see that it finds them. Names any add-on holds, such as CreateFileW, an import of
the C runtime, say nothing and are not listed.
"""
import sys
from pathlib import Path

TEXTS = ["PhantomFX-automation", "Costs on file", "SteamRemoteStorage", "InspectPixels", "GAME_SHADOW", "g_ShadowMapAlone", "sun-light-game-shadow", "left out:", "dropped by the mesh rules",
    "Debug case", "PhantomFX-marks", "Hold for original", "reveal_now", "case_done", "command files", "TppPlayer", "TppMission", "ReserveMissionClear", "Automation", "=automation", "probe_text"]
WIDE_TEXTS = ["steam_api64.dll"]


def main():
    path = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / "build" / "release" / "PhantomFX.addon64")
    data = path.read_bytes()
    found = [text for text in TEXTS if text.encode() in data] + [text for text in WIDE_TEXTS if text.encode("utf-16-le") in data]
    print(f"{path}: {len(data):,} bytes")
    if found:
        sys.exit("Holds the dev tools: " + ", ".join(found))

    print("None of the dev tools in it.")


if __name__ == "__main__":
    main()
