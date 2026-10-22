# SPDX-License-Identifier: GPL-2.0-only
"""Host command assignment, focus movement and segmented editing without LVGL."""

from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]


def test_screen_model_commands_focus_and_segmented_editing(tmp_path: Path) -> None:
    """Commands stay reachable; date and elapsed duration retain their input rules."""
    executable = tmp_path / "screen-model"
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            str(ROOT / "tests/host_tool/gui/fplinux-ui-screen-model.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-screen-model.c"),
            "-o",
            str(executable),
        ],
        name="compile screen model",
        timeout=30,
        check=True,
    )
    run_process([str(executable)], name="run screen model", timeout=10, check=True)
