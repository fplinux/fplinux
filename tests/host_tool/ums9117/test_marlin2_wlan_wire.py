# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for Marlin2 wireless command, data and event wire contracts."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/drivers/mmc/host"
FIXTURES = ROOT / "tests/host_tool/ums9117"


def test_commands_data_credits_and_ap_beacon_bytes(tmp_path: Path) -> None:
    """Link the actual codec; no cfg80211, GPIO, SDIO or phone path executes."""
    executable = tmp_path / "marlin2-wlan-wire"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-UNDEBUG",
            f"-I{FIXTURES / 'fm-compat'}",
            f"-I{FIXTURES / 'bluetooth-compat'}",
            f"-I{KERNEL}",
            str(FIXTURES / "marlin2-wlan-wire.c"),
            str(KERNEL / "marlin2-wlan-wire.c"),
        ],
        name="compile Marlin2 wireless wire codec host checks",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run Marlin2 wireless wire codec host checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "marlin2 WLAN wire host cases passed\n"
