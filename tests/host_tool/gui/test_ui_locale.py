# SPDX-License-Identifier: GPL-2.0-only
"""Host date and number formatting with controlled language-file peers."""

from __future__ import annotations

from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]


def test_dates_and_numbers_follow_system_language(tmp_path: Path) -> None:
    executable = tmp_path / "locale"
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffunction-sections",
            "-Wl,--gc-sections",
            "-I",
            str(ROOT / "include/fplinux"),
            str(Path(__file__).with_name("fplinux-ui-locale.c")),
            str(ROOT / "lib/fplinux/fplinux-ui-locale.c"),
            str(ROOT / "lib/fplinux/fplinux-text-input.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            "-o",
            str(executable),
        ],
        name="compile UI locale harness",
        timeout=30,
        check=True,
    )
    run_process(
        [str(executable)],
        name="run UI locale harness",
        timeout=10,
        check=True,
    )
