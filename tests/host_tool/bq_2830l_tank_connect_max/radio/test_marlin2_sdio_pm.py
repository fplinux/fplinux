# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for retained SDIO power during system suspend."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/overlay/drivers/mmc/host/bq2830l-radio"
FIXTURES = ROOT / "tests/host_tool/bq_2830l_tank_connect_max/radio"


def test_persistent_sdio_suspend_retains_power_each_cycle_and_reports_unsupported_host(
    tmp_path: Path,
) -> None:
    """Link the driver; device state and the external SDIO PM API are doubles."""
    executable = tmp_path / "marlin2-sdio-pm"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-UNDEBUG",
            "-DCONFIG_BQ2830L_BLUETOOTH=1",
            f"-I{FIXTURES / 'marlin2-sdio-write-compat'}",
            f"-I{KERNEL}",
            str(FIXTURES / "marlin2-sdio-pm.c"),
            str(KERNEL / "marlin2-sdio.c"),
        ],
        name="compile Marlin2 SDIO PM host component checks",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run Marlin2 SDIO PM host component checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Marlin2 SDIO PM host component cases passed\n"
