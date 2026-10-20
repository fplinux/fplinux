# SPDX-License-Identifier: GPL-2.0-only
"""Host lifecycle checks with controlled work scheduling and media boundaries."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

DRIVER = ROOT / "platforms/ums9117/linux/overlay/drivers/media/platform/ums9117"
MEDIA = ROOT / "tests/host_tool/media"
FIXTURES = ROOT / "tests/fixtures/ums9117-kernel"


def test_dcam_unbind_stops_capture_before_open_file_release(tmp_path: Path) -> None:
    """Unbind returns buffers and releases streaming resources before close."""
    executable = tmp_path / "dcam-capture-lifecycle"
    compile_c(
        executable,
        [
            "-std=gnu11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            f"-I{MEDIA / 'dcam-capture-compat'}",
            f"-I{FIXTURES / 'lcm-compat'}",
            f"-I{FIXTURES / 'bluetooth-compat'}",
            f"-I{DRIVER}",
            str(MEDIA / "dcam-capture-lifecycle.c"),
            str(DRIVER / "ums9117-dcam-capture.c"),
        ],
        name="compile DCAM capture lifecycle harness",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run DCAM capture lifecycle harness",
        timeout=10,
        check=True,
    )
    assert result.stdout == "DCAM capture lifecycle checks passed\n"
