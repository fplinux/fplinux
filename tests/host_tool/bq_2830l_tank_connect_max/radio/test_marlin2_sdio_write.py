# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked write and cleanup behavior at controlled SDIO boundaries."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/overlay/drivers/mmc/host/bq2830l-radio"
FIXTURES = ROOT / "tests/host_tool/bq_2830l_tank_connect_max/radio"


@pytest.fixture(scope="module")
def sdio_executable(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "marlin2-sdio-write") as directory:
        executable = directory / "marlin2-sdio-write"
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
        yield executable


def test_bluetooth_direct_write_submits_complete_frame_through_fixed_port_operation(
    sdio_executable: Path,
) -> None:
    """Link the owner; SDIO APIs/device are doubles, without core/controller/HCI execution."""
    result = run_process(
        [str(sdio_executable)],
        name="run Marlin2 SDIO write owner host checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Marlin2 SDIO write owner host cases passed\n"


def test_sdio_probe_and_remove_restore_state_and_preserve_first_error(
    sdio_executable: Path,
) -> None:
    """Link the driver with controlled firmware, board, clock and SDIO boundaries."""
    result = run_process(
        [str(sdio_executable), "cleanup"],
        name="run Marlin2 SDIO cleanup host checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Marlin2 SDIO cleanup host cases passed\n"
