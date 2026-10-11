# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for Marlin2 packet framing and capability reply decoding."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/drivers/mmc/host"
FIXTURES = ROOT / "tests/host_tool/ums9117"


def test_get_info_reply_and_packet_boundaries(tmp_path: Path) -> None:
    """Link the actual wire codec; kernel adapters do not execute GPIO or SDIO."""
    executable = tmp_path / "marlin2-packet"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-UNDEBUG",
            f"-I{FIXTURES / 'marlin2-packet-compat'}",
            f"-I{FIXTURES / 'fm-compat'}",
            f"-I{FIXTURES / 'bluetooth-compat'}",
            f"-I{KERNEL}",
            str(FIXTURES / "marlin2-packet.c"),
            str(KERNEL / "marlin2-packet-codec.c"),
            str(KERNEL / "marlin2-transport-wire.c"),
        ],
        name="compile Marlin2 packet codec host checks",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run Marlin2 packet codec host checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Marlin2 packet codec host cases passed\n"
