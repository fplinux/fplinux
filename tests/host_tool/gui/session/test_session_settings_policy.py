# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for settings, time, language and file policy."""

from pathlib import Path

from tests import ROOT
from tests.process import run_process

APORT = ROOT / "alpine/aports/fplinux-session"


def test_settings_time_language_and_file_policy(tmp_path: Path) -> None:
    executable = tmp_path / "session-settings-policy-test"
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
            str(Path(__file__).with_name("fplinux-session-settings-policy-test.c")),
            str(APORT / "session-settings.c"),
            str(ROOT / "lib/fplinux-shared/fplinux-settings-values.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            "-o",
            str(executable),
        ],
        name="compile settings policy host harness",
        timeout=30,
        check=True,
    )
    run_process(
        [str(executable)],
        name="run settings policy host harness",
        timeout=10,
        check=True,
    )
