# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked Bluetooth write owner at an explicit SDIO operation boundary."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/drivers/mmc/host"
FIXTURES = ROOT / "tests/host_tool/ums9117"


def test_bluetooth_direct_write_submits_complete_frame_through_fixed_port_operation(
    tmp_path: Path,
) -> None:
    """Link the owner; SDIO APIs/device are doubles, without core/controller/HCI execution."""
    executable = tmp_path / "marlin2-sdio-write"
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
            "-ffunction-sections",
            "-fdata-sections",
            f"-I{FIXTURES / 'marlin2-sdio-write-compat'}",
            f"-I{KERNEL}",
            str(FIXTURES / "marlin2-sdio-write.c"),
            str(KERNEL / "marlin2-sdio.c"),
            "-Wl,--gc-sections",
        ],
        name="compile Marlin2 SDIO write owner host checks",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run Marlin2 SDIO write owner host checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Marlin2 SDIO write owner host cases passed\n"
