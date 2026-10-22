# SPDX-License-Identifier: GPL-2.0-only
"""Injected wall and monotonic time through the production Clock model."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_clock_time_events_and_user_values(tmp_path: Path) -> None:
    """Literal dated schedules, countdowns and laps retain their distinct clocks."""
    aport = ROOT / "alpine/aports/fplinux-clocks"
    executable = tmp_path / "clock-model"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{aport}",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/clock-model-test.c"),
            str(aport / "clock-model.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
        ],
        name="compile Clock model behavior harness",
    )
    result = run_process(
        [str(executable)], name="run Clock model behavior", timeout=10, check=True
    )
    assert result.stdout == "clock-model: time, events and user values passed\n"
    assert result.stderr == ""
