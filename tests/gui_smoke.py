#!/usr/bin/env python3
"""Run the GUI against a tiny fixture with SDL's dummy video driver."""

import os
import subprocess
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: gui_smoke.py <GUI executable or launcher>")

    build_directory = Path("build")
    build_directory.mkdir(exist_ok=True)
    log_path = build_directory / "gui-smoke.log"
    screenshot_path = build_directory / "gui-smoke.bmp"
    log_path.write_text("Jun  4 15:16:01 host app[42]: startup\n", encoding="utf-8")
    if screenshot_path.exists():
        screenshot_path.unlink()

    environment = os.environ.copy()
    environment["SDL_VIDEODRIVER"] = "dummy"
    environment["GLOGVIEW_GUI_CAPTURE"] = str(screenshot_path)
    subprocess.run(
        [
            sys.argv[1],
            str(log_path),
            "--format",
            "syslog3164",
            "--syslog-year",
            "2026",
            "--syslog-timezone",
            "+02:00",
        ],
        check=True,
        env=environment,
        timeout=30,
    )
    if not screenshot_path.is_file() or screenshot_path.stat().st_size == 0:
        raise SystemExit("GUI smoke test did not produce a rendered frame")


if __name__ == "__main__":
    main()
