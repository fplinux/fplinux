# SPDX-License-Identifier: GPL-2.0-only
"""Literal arithmetic and editing outcomes through the production Calculator core."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_calculator_arithmetic_editing_and_saved_continuation(tmp_path: Path) -> None:
    """Saved partial operands and results continue to independently known answers."""
    aport = ROOT / "alpine/aports/fplinux-calculator"
    executable = tmp_path / "calculator-model"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{aport}",
            str(ROOT / "tests/host_tool/gui/calculator-model.c"),
            str(aport / "calculator.c"),
            "-lm",
        ],
        name="compile the Calculator core with literal behavior checks",
    )
    result = run_process(
        [str(executable)], name="run Calculator core behavior", timeout=10, check=True
    )
    assert result.stdout == "calculator-model: arithmetic, editing and saved continuation passed\n"
    assert result.stderr == ""
