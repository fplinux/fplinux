# SPDX-License-Identifier: GPL-2.0-only
"""Exercise CPU clock reads and bounded parent control with register doubles."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/clocks/cpu-clock"
DRIVER = ROOT / "platforms/sc9820e/linux/overlay/drivers/clk/sprd/sc9820e-cpu-clock.c"


@pytest.fixture(scope="module")
def cpu_clock_component(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link real probe and rate callbacks separately from kernel API doubles."""
    with c_build_directory(tmp_path_factory, "sc9820e_cpu_clock") as temporary:
        executable = temporary / "sc9820e-cpu-clock"
        result = compile_c(
            executable,
            [
                "-O2",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{FIXTURES}",
                f"-I{FIXTURES / 'compat'}",
                f"-I{ROOT / 'platforms/sc9820e/linux/overlay/include'}",
                str(FIXTURES / "cpu-clock-host.c"),
                str(DRIVER),
            ],
            name="compile SC9820E CPU parent clock host component",
            check=False,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        yield executable


@pytest.mark.parametrize(
    "scenario",
    [
        "rates",
        "probe-errors",
        "parent-transitions",
        "parent-unknown-state",
        "parent-errors",
        "parent-probe-errors",
        "twpll-output",
        "cpu-output-selection",
    ],
)
def test_cpu_clock_reads_and_parent_transitions_preserve_contract(
    cpu_clock_component: Path, scenario: str
) -> None:
    """Callbacks preserve reads, parent boundaries, real errors and cleanup."""
    result = run_process(
        [str(cpu_clock_component), scenario],
        name=f"run SC9820E CPU clock {scenario} host component",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
