# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for display, keyguard and input policy."""

from pathlib import Path

from tests import ROOT
from tests.process import run_process

APORT = ROOT / "alpine/aports/fplinux-session"


def test_display_keyguard_and_input_policy(tmp_path: Path) -> None:
    executable = tmp_path / "session-power-test"
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            "-I",
            str(APORT),
            str(Path(__file__).with_name("fplinux-session-power-test.c")),
            str(APORT / "session-power.c"),
            "-o",
            str(executable),
        ],
        name="compile display and input policy host harness",
        timeout=30,
        check=True,
    )
    run_process(
        [str(executable)],
        name="run display and input policy host harness",
        timeout=10,
        check=True,
    )
