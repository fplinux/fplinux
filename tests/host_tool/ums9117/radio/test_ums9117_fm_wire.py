# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked FM command and reply contracts shared by both radio transports."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

PLATFORM = ROOT / "platforms/ums9117/linux/overlay"
FIXTURES = ROOT / "tests/host_tool/ums9117/radio"
KERNEL_COMPAT = ROOT / "tests/fixtures/ums9117-kernel/bluetooth-compat"


def test_fm_commands_split_replies_seek_order_and_consumer_capacities(tmp_path: Path) -> None:
    """Link the common codec; transport admission, V4L2 and hardware do not execute."""
    executable = tmp_path / "ums9117-fm-wire"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-UNDEBUG",
            f"-I{KERNEL_COMPAT}",
            f"-I{PLATFORM / 'include'}",
            str(FIXTURES / "ums9117-fm-wire.c"),
            str(PLATFORM / "lib/ums9117-fm-wire.c"),
        ],
        name="compile shared UMS9117 FM wire host checks",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run shared UMS9117 FM wire host checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == "UMS9117 FM wire host cases passed\n"
