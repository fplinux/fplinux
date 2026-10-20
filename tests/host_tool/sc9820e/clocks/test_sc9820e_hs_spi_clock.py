# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the native HS_SPI callbacks and the SPI rate calculator."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/clocks"
DRIVERS = ROOT / "platforms/sc9820e/linux/overlay/drivers"


@pytest.fixture(scope="module")
def hs_spi_clock_component(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link real clock callbacks with the existing native clock API peer."""
    with c_build_directory(tmp_path_factory, "sc9820e_hs_spi_clock") as temporary:
        executable = temporary / "sc9820e-hs-spi-clock"
        result = compile_c(
            executable,
            [
                "-O2",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{FIXTURES / 'cpu-clock'}",
                f"-I{FIXTURES / 'cpu-clock/compat'}",
                f"-I{DRIVERS / 'spi'}",
                str(FIXTURES / "hs-spi-clock.c"),
                str(DRIVERS / "clk/sprd/sc9820e-hs-spi-clock.c"),
            ],
            name="compile SC9820E HS_SPI clock host component",
            check=False,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        yield executable


@pytest.mark.parametrize(
    "scenario",
    [
        "mux-divider",
        "parent-rate-request",
        "write-errors",
        "probe-errors",
        "probe-ownership",
        "spi-rates",
    ],
)
def test_hs_spi_source_and_rate_preserve_register_contract(
    hs_spi_clock_component: Path, scenario: str
) -> None:
    """Callbacks select the requested parent and preserve unrelated controls."""
    result = run_process(
        [str(hs_spi_clock_component), scenario],
        name=f"run SC9820E HS_SPI clock {scenario} host component",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
