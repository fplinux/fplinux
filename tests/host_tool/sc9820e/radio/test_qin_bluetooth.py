# SPDX-License-Identifier: GPL-2.0-only
"""Link the Qin Bluetooth codec to literal H4 command and reply consumers."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/radio"
SOURCE = ROOT / "targets/xiaomi-qin-1s-plus/linux/overlay/drivers/bluetooth"


def test_bt_wire_preserves_exact_setup_replies_and_transport_framing(tmp_path: Path) -> None:
    """Exercise the production codec without executing native HCI or the controller."""
    executable = tmp_path / "qin-bt-protocol"
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
            str(FIXTURES / "qin-bt-protocol.c"),
            str(SOURCE / "qin1splus-bt-protocol.c"),
        ],
        name="compile Qin Bluetooth protocol host checks",
    )
    result = run_process(
        [str(executable)],
        name="run Qin Bluetooth protocol host checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Qin Bluetooth protocol host cases passed\n"
