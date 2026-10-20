# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for display, keyguard and persistent settings."""

from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]
APORT = ROOT / "alpine/aports/fplinux-session"


def test_display_keyguard_and_settings_policy(tmp_path: Path) -> None:
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
            str(APORT / "session-settings.c"),
            str(ROOT / "lib/fplinux/fplinux-settings-values.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            "-o",
            str(executable),
        ],
        name="compile display and settings host harness",
        timeout=30,
        check=True,
    )
    run_process(
        [str(executable)],
        name="run display and settings host harness",
        timeout=10,
        check=True,
    )
