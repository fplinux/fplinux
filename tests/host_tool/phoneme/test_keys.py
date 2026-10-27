# SPDX-License-Identifier: GPL-2.0-only
"""Real Canvas key producer with controlled MIDP queue and timer boundaries."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_input_cancel_releases_only_its_seat_and_preserves_other_repeat(tmp_path: Path) -> None:
    port = ROOT / "alpine/aports/fplinux-phoneme"
    harness = ROOT / "tests/host_tool/phoneme"
    executable = tmp_path / "keys"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{harness / 'compat'}",
            f"-I{ROOT / 'include'}",
            f"-I{port / 'vm'}",
            f"-I{port / 'port/canvas'}",
            str(harness / "keys-test.c"),
            str(port / "port/canvas/fplinux-pm-keys.c"),
            str(port / "port/canvas/fplinux-pm-canvas-core.c"),
        ],
        name="compile phoneME Canvas key producer with controlled queue and clock",
    )
    result = run_process(
        [str(executable)], name="phoneME separate phone and keyboard cancellation", timeout=10
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "keys: seat cancellation preserves other held input\n"
    assert result.stderr == ""
