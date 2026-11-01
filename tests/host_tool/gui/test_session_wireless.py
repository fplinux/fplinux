# SPDX-License-Identifier: GPL-2.0-only
"""Session wireless policy with a controlled Linux network boundary."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_wireless_state_requests_and_external_changes(tmp_path: Path) -> None:
    executable = tmp_path / "session-wireless"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            f"-I{ROOT / 'alpine/aports/fplinux-session'}",
            str(ROOT / "tests/host_tool/gui/session-wireless-test.c"),
            str(ROOT / "alpine/aports/fplinux-session/session-wireless.c"),
            "-Wl,--wrap=if_nameindex",
            "-Wl,--wrap=if_freenameindex",
            "-Wl,--wrap=stat",
            "-Wl,--wrap=ioctl",
            "-Wl,--wrap=socket",
            "-Wl,--wrap=bind",
        ],
        name="compile session wireless backend peer",
    )
    result = run_process(
        [str(executable)],
        name="wireless administrative state and asynchronous requests",
        timeout=15,
        check=True,
    )
    assert result.stdout == "session wireless: OK\n"
