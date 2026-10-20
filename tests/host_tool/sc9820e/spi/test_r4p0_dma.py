# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production DMA owner with a controlled DMAengine peer."""

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
HEADER = ROOT / "platforms/sc9820e/linux/overlay/drivers/spi"


@pytest.fixture(scope="module")
def dma_peer(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Compile the real header-only owner against the native API peer."""
    with c_build_directory(tmp_path_factory, "r4p0_dma_peer") as temporary:
        executable = temporary / "r4p0-dma-peer"
        result = compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{FIXTURES / 'compat'}",
                f"-I{HEADER}",
                str(FIXTURES / "r4p0-dma-peer.c"),
            ],
            name="compile r4p0 DMA resource host component",
            check=False,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        yield executable


@pytest.mark.parametrize(
    "scenario",
    [
        "width8",
        "width16",
        "width32",
        "timeout-stop",
        "completion-error",
        "stop-error",
        "submit-error",
        "invalid-length",
        "request-error",
        "allocation-error",
    ],
)
def test_dma_owns_its_buffer_until_successful_stop(dma_peer: Path, scenario: str) -> None:
    """Check copied bytes, bus widths and ownership across completion/stop errors."""
    result = run_process(
        [str(dma_peer), scenario],
        name=f"r4p0 DMA resource peer: {scenario}",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
