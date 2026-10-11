# SPDX-License-Identifier: GPL-2.0-only
"""Host MMC-driver slot policy with fake device properties, MMIO and ADI."""

from __future__ import annotations

from typing import TYPE_CHECKING, ClassVar

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

UBOOT = ROOT / "platforms/ums9117/uboot"
HARNESS = ROOT / "tests/host_tool/ums9117"


class Ums9117MmcSlotPolicyTests:
    """Run registered driver callbacks with the linked slot and controller logic."""

    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_peer(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[None]:
        with c_build_directory(tmp_path_factory, "ums9117_mmc") as temporary:
            cls.executable = temporary / "ums9117-mmc"
            compile_c(
                cls.executable,
                [
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-DCONFIG_SYS_DCACHE_OFF",
                    f"-I{HARNESS / 'mmc-compat'}",
                    f"-I{HARNESS / 'ums9117-sdio-compat'}",
                    f"-I{UBOOT}",
                    f"-I{ROOT / 'platforms/ums9117/common'}",
                    str(HARNESS / "ums9117-mmc.c"),
                    str(UBOOT / "ums9117-mmc.c"),
                    str(UBOOT / "ums9117-sdio-slot.c"),
                    str(UBOOT / "ums9117-sdio-core.c"),
                ],
                name="compile MMC-driver slot policy harness",
            )
            yield

    @pytest.mark.parametrize(
        "scenario",
        [
            "fixed-no-detect",
            "fixed-busy-detect",
            "fixed-invalid-voltage",
            "removable-present",
            "removable-absent",
        ],
    )
    def test_slot_presence_initialization_and_release(self, scenario: str) -> None:
        """Fixed slots need no detection signal; removable slots retain detection."""
        result = run_process(
            [str(self.executable), scenario],
            name="run MMC-driver slot policy harness",
            timeout=30,
        )
        assert result.returncode == 0, result.stdout + result.stderr
