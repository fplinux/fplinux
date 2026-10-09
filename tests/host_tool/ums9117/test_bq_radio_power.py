# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for radio supply ownership across channel lifetimes."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/drivers/mmc/host"
FIXTURES = ROOT / "tests/host_tool/ums9117"


def test_radio_supplies_restore_after_shared_lifetime_and_failed_start(tmp_path: Path) -> None:
    """Link the owner and transport; hardware and consumer registration are doubles."""
    executable = tmp_path / "bq-radio-power"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-Wno-sign-compare",
            "-UNDEBUG",
            "-pthread",
            f"-I{FIXTURES / 'bq-radio-power-compat'}",
            f"-I{FIXTURES / 'fm-compat'}",
            f"-I{FIXTURES / 'bluetooth-compat'}",
            f"-I{KERNEL}",
            str(FIXTURES / "bq-radio-power.c"),
            str(KERNEL / "bq2830l-radio-power.c"),
            str(KERNEL / "marlin2-transport.c"),
            str(KERNEL / "marlin2-transport-wire.c"),
            str(KERNEL / "marlin2-packet-codec.c"),
        ],
        name="compile BQ radio power host component checks",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run BQ radio power host component checks",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("BQ radio power host component cases passed\n")
