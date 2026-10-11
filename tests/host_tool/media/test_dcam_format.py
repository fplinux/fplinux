# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for DCAM capture geometry and transport configuration."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "platforms/ums9117/linux/drivers/media/platform/ums9117"
HARNESS = ROOT / "tests/host_tool/media/dcam-format.c"
COMPAT = ROOT / "tests/host_tool/media/jpeg-codec-compat"
KCONFIG_COMPAT = ROOT / "tests/host_tool/media/dcam-format-compat"
SERIAL = "CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0"
ONE_BIT = "CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0_ONE_BIT"
TWO_BIT = "CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0_TWO_BIT"
PARALLEL = "CONFIG_VIDEO_UMS9117_DCAM_PARALLEL"


@pytest.mark.parametrize(
    ("enabled", "expected_support"),
    [
        pytest.param((), 0, id="no-camera-input"),
        pytest.param((SERIAL,), 0, id="serial-without-data-profile"),
        pytest.param((SERIAL, ONE_BIT), 1, id="one-bit-only"),
        pytest.param((SERIAL, TWO_BIT), 2, id="two-bit-only"),
        pytest.param((PARALLEL,), 4, id="parallel-only"),
        pytest.param((SERIAL, ONE_BIT, TWO_BIT, PARALLEL), 7, id="all-inputs"),
    ],
)
def test_capture_geometry_and_transport_fields(
    tmp_path: Path, enabled: tuple[str, ...], expected_support: int
) -> None:
    """Check capture fields against literal geometry and transport examples."""
    executable = tmp_path / "dcam-format"
    compile_c(
        executable,
        [
            "-O2",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{KCONFIG_COMPAT}",
            f"-I{COMPAT}",
            f"-I{KERNEL}",
            *(f"-D{option}=1" for option in enabled),
            str(HARNESS),
            str(KERNEL / "ums9117-dcam-format.c"),
        ],
        name="compile DCAM format host checks",
        check=True,
    )
    result = run_process(
        [str(executable), str(expected_support)],
        name="run DCAM format host checks",
        timeout=30,
        check=True,
    )
    assert result.stdout == "DCAM format checks passed\n"
