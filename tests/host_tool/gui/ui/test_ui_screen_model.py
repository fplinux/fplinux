# SPDX-License-Identifier: GPL-2.0-only
"""Host command assignment, focus movement and segmented editing without LVGL."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


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
            str(ROOT / "tests/host_tool/gui/ui/fplinux-ui-screen-model.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-screen-model.c"),
            "-o",
            str(executable),
        ],
        name="compile screen model",
        timeout=30,
        check=True,
    )
    run_process([str(executable)], name="run screen model", timeout=10, check=True)
