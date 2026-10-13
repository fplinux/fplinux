# SPDX-License-Identifier: GPL-2.0-only
"""Linked SPI transport checks with fake MMIO, resources and virtual time."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

KERNEL = ROOT / "platforms/ums9117/linux/overlay/drivers/gpu/drm/ums9117"
FIXTURES = ROOT / "tests/host_tool/ums9117/display"
KERNEL_COMPAT = ROOT / "tests/fixtures/ums9117-kernel"


@pytest.fixture(scope="module")
def spi_host_component(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Compile the production SPI file as a separate translation unit."""
    with c_build_directory(tmp_path_factory, "ums9117_spi") as temporary:
        executable = temporary / "ums9117-spi"
        compilation = compile_c(
            executable,
            [
                "-O2",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{FIXTURES / 'spi-compat'}",
                f"-I{KERNEL_COMPAT / 'lcm-compat'}",
                f"-I{KERNEL}",
                str(FIXTURES / "ums9117-spi.c"),
                str(KERNEL / "ums9117-drm-spi.c"),
            ],
            name="compile UMS9117 SPI host component",
            check=False,
        )
        assert compilation.returncode == 0, compilation.stderr
        yield executable


@pytest.mark.parametrize(
    "scenario",
    ["raw16-dcs", "default-dcs", "raw16-frame", "default-frame", "raw16-timeout"],
)
def test_spi_command_and_pixel_handoff(spi_host_component: Path, scenario: str) -> None:
    """Check wire bytes, command/data selection, CS ownership and pixel counts."""
    result = run_process(
        [str(spi_host_component), scenario],
        name=f"run UMS9117 SPI host component: {scenario}",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
