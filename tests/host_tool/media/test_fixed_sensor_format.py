# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for the sensors' shared fixed-format callbacks."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "platforms/ums9117/linux/overlay/drivers/media/i2c"
COMPAT = ROOT / "tests/fixtures/ums9117-kernel/sensor-compat"
HARNESS = ROOT / "tests/host_tool/media/fixed-sensor-format.c"


def test_fixed_sensor_format_callbacks_preserve_state_and_validation(tmp_path: Path) -> None:
    """Check literal formats, rejected pads/codes and request normalization."""
    executable = tmp_path / "fixed-sensor-format"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{COMPAT}",
            f"-I{KERNEL}",
            str(HARNESS),
        ],
        name="compile fixed sensor format callbacks",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run fixed sensor format callbacks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Fixed sensor format checks passed\n"
