# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked Bluetooth setup buffer ownership at the transport boundary."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/drivers/mmc/host"
FIXTURES = ROOT / "tests/host_tool/ums9117"


def test_setup_submits_dma_safe_buffers_and_releases_them_after_transfer(tmp_path: Path) -> None:
    """HCI and transport are fakes; real setup and H4 code run without physical DMA."""
    executable = tmp_path / "marlin2-bluetooth-setup"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-UNDEBUG",
            f"-I{FIXTURES / 'marlin2-bluetooth-setup-compat'}",
            f"-I{FIXTURES / 'bluetooth-compat'}",
            f"-I{KERNEL}",
            str(FIXTURES / "marlin2-bluetooth-setup.c"),
            str(KERNEL / "marlin2-bluetooth.c"),
            str(KERNEL / "marlin2-bluetooth-wire.c"),
        ],
        name="compile Marlin2 Bluetooth setup host buffer checks",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run Marlin2 Bluetooth setup host buffer checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Marlin2 Bluetooth setup host buffer cases passed\n"
