# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for radio supply ownership across channel lifetimes."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "platforms/ums9117/linux/overlay/drivers/mmc/host/marlin2"
FIXTURES = ROOT / "tests/host_tool/bq_2830l_tank_connect_max/radio"
KERNEL_COMPAT = ROOT / "tests/fixtures/ums9117-kernel"
BQ = ROOT / "targets/bq-2830l-tank-connect-max/linux/overlay/drivers/mmc/host/marlin2"
ITEL = ROOT / "targets/itel-neo-r60-plus/linux/overlay/drivers/mmc/host/marlin2"


@pytest.mark.parametrize("bq_enabled", [True, False], ids=["all-boards", "itel-only"])
def test_radio_supplies_restore_after_shared_lifetime_and_failed_start(
    tmp_path: Path, *, bq_enabled: bool
) -> None:
    """Link the enabled boards and transport with hardware and interface doubles."""
    executable = tmp_path / "bq-radio-power"
    bq_sources = [str(BQ / "bq2830l-marlin2.c")] if bq_enabled else []
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
            f"-DCONFIG_MACH_BQ_2830L_TANK_CONNECT_MAX={int(bq_enabled)}",
            "-DCONFIG_MACH_ITEL_NEO_R60_PLUS=1",
            f"-I{FIXTURES / 'bq-radio-power-compat'}",
            f"-I{KERNEL_COMPAT / 'fm-compat'}",
            f"-I{KERNEL_COMPAT / 'bluetooth-compat'}",
            f"-I{KERNEL}",
            f"-I{BQ}",
            f"-I{ITEL}",
            str(FIXTURES / "bq-radio-power.c"),
            str(KERNEL / "marlin2-radio.c"),
            str(KERNEL / "marlin2-transport.c"),
            str(KERNEL / "marlin2-transport-wire.c"),
            str(KERNEL / "marlin2-packet-codec.c"),
            str(ITEL / "itel-neo-r60-marlin2.c"),
            *bq_sources,
        ],
        name="compile BQ radio power host component checks",
        check=True,
    )
    result = run_process(
        [str(executable), *([] if bq_enabled else ["itel-only"])],
        name="run BQ radio power host component checks",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("Marlin2 radio power host component cases passed\n")
