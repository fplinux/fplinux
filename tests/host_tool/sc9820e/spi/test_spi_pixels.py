# SPDX-License-Identifier: GPL-2.0-only
"""Exercise RGB565 wire packing and SPI word accounting with native API peers."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/spi"
OVERLAY = ROOT / "platforms/sc9820e/linux/overlay/drivers"


@pytest.fixture(scope="module")
def pixel_peer(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Compile production header helpers against bounded SPI/allocation peers."""
    with c_build_directory(tmp_path_factory, "spi_pixel_peer") as temporary:
        executable = temporary / "spi-pixel-peer"
        result = compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{FIXTURES / 'pixel-compat'}",
                f"-I{OVERLAY / 'gpu/drm'}",
                f"-I{OVERLAY / 'spi'}",
                str(FIXTURES / "spi-pixel-peer.c"),
            ],
            name="compile RGB565 SPI host component",
            check=False,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        yield executable


@pytest.mark.parametrize(
    "scenario",
    [
        "wire32",
        "unaligned32",
        "error32",
        "allocation-error",
        "odd16",
        "single16",
        "no32",
        "bytes8",
        "lsb16",
        "limit16",
        "word-control",
        "word-counts",
    ],
)
def test_rgb565_spi_preserves_wire_and_source(pixel_peer: Path, scenario: str) -> None:
    """Check native wire words, odd clips, source lifetime and length encoding."""
    result = run_process(
        [str(pixel_peer), scenario],
        name=f"RGB565 SPI host peer: {scenario}",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
