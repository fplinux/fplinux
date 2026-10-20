# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the GPU clock and unchanged core supply through their driver APIs."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/gpu"
DRIVERS = ROOT / "platforms/sc9820e/linux/overlay/drivers"
SHARED = ROOT / "platforms/shared/linux/overlay/drivers/regulator"


@pytest.fixture(scope="module")
def gpu_clock_component(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Replace MMIO and CCF boundaries while retaining both production drivers."""
    with c_build_directory(tmp_path_factory, "gpu-clock") as directory:
        executable = directory / "gpu-clock-host"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(FIXTURES / "compat"),
                "-I",
                str(SHARED),
                str(FIXTURES / "gpu-clock-host.c"),
                str(DRIVERS / "clk/sprd/sc9820e-gpu-clock.c"),
                str(DRIVERS / "regulator/sc2721-regulator.c"),
                str(SHARED / "sc27xx-regulator-core.c"),
            ],
            name="compile the GPU clock and core supply components",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario",
    [
        "supply",
        "lifetime",
        "failure",
        "registration",
        "target-voltage",
        "assigned-rate",
        "rate-admission",
        "rate-observation",
        "selected-rate-failure",
    ],
)
def test_gpu_clock_preserves_shared_resources_and_bounds_control_access(
    gpu_clock_component: Path, scenario: str
) -> None:
    """Observe driver effects; electrical readiness and native jobs need hardware."""
    run_process(
        [str(gpu_clock_component), scenario],
        name=f"exercise GPU clock component: {scenario}",
        timeout=5,
        check=True,
    )
