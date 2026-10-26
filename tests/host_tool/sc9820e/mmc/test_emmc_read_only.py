# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the eMMC command boundary with host MMC request structures."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/mmc"
HOST = ROOT / "platforms/sc9820e/linux/overlay/drivers/mmc/host"


@pytest.fixture(scope="module")
def emmc_read_only(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Compile the same policy header consumed by the r10 host driver."""
    with c_build_directory(tmp_path_factory, "sc9820e-emmc-ro") as directory:
        executable = directory / "emmc-ro"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{FIXTURES / 'emmc-ro-compat'}",
                f"-I{HOST}",
                str(FIXTURES / "emmc-ro.c"),
            ],
            name="compile the eMMC command boundary host component",
        )
        yield executable


@pytest.mark.parametrize("scenario", ["reads", "blocked", "configuration"])
def test_emmc_requests_preserve_read_only_user_access(emmc_read_only: Path, scenario: str) -> None:
    """Allow reads and bounded volatile settings while rejecting persistent writes."""
    run_process(
        [str(emmc_read_only), scenario],
        name=f"exercise eMMC read-only command policy: {scenario}",
        timeout=5,
        check=True,
    )
