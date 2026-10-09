# SPDX-License-Identifier: GPL-2.0-only
"""Host-component characterization of slot ownership and power policy."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

UBOOT = ROOT / "platforms/ums9117/uboot"
HARNESS = ROOT / "tests/host_tool/ums9117/ums9117-sdio-slot.c"
COMPAT = ROOT / "tests/host_tool/ums9117/ums9117-sdio-compat"


class Ums9117SdioSlotTests:
    """Link production slot logic to fake MMIO/ADI; no device is exercised."""

    def test_slot_ownership_and_power_policy(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Preserve VSEL admission, EIC ownership and the power-down bit policy."""
        with c_build_directory(tmp_path_factory, "ums9117_sdio_slot") as temporary:
            executable = temporary / "ums9117-sdio-slot"
            compile_c(
                executable,
                [
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{COMPAT}",
                    f"-I{UBOOT}",
                    str(HARNESS),
                    str(UBOOT / "ums9117-sdio-slot.c"),
                    str(UBOOT / "ums9117-sdio-core.c"),
                ],
                name="compile UMS9117 SDIO slot harness",
                check=True,
            )
            result = run_process(
                [str(executable)],
                name="run UMS9117 SDIO slot harness",
                timeout=30,
            )
        assert (result.returncode) == (0), f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
