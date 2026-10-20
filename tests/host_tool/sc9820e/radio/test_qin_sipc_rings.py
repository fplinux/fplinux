# SPDX-License-Identifier: GPL-2.0-only
"""Link the Qin SIPC ring owner to literal shared-memory byte consumers."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/radio"
SOURCE = ROOT / "targets/xiaomi-qin-1s-plus/linux/overlay/drivers/remoteproc"


def test_bt_rings_preserve_copy_wrap_backpressure_and_notification_edges(tmp_path: Path) -> None:
    """Replace MMIO accessors; kernel concurrency and device coherency do not execute."""
    executable = tmp_path / "qin-sipc-rings"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-UNDEBUG",
            "-I",
            str(FIXTURES / "compat"),
            "-I",
            str(SOURCE),
            str(FIXTURES / "qin-sipc-rings.c"),
            str(SOURCE / "qin1splus-sipc-layout.c"),
        ],
        name="compile Qin SIPC BT ring host checks",
    )
    result = run_process(
        [str(executable)],
        name="run Qin SIPC BT ring host checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Qin SIPC BT ring host cases passed\n"
