# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks using temporary power-supply attribute files."""

from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]
APORT = ROOT / "alpine/aports/fplinux-session"


def test_supply_reading_estimate_and_alert_cycles(tmp_path: Path) -> None:
    executable = tmp_path / "session-battery-test"
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(APORT),
            str(Path(__file__).with_name("fplinux-session-battery-test.c")),
            str(APORT / "session-battery.c"),
            "-o",
            str(executable),
        ],
        name="compile battery host harness",
        timeout=30,
        check=True,
    )
    run_process(
        [str(executable)],
        name="run battery host harness",
        timeout=10,
        check=True,
    )
