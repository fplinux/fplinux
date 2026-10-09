# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for FM error recovery through the shared radio owner."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/drivers/mmc/host"
FIXTURES = ROOT / "tests/host_tool/ums9117"


def test_failed_seek_recovers_on_fresh_runtime_and_preserves_other_consumers(
    tmp_path: Path,
) -> None:
    """Link FM callbacks and transport; V4L2, board, IRQ and SDIO are explicit doubles."""
    executable = tmp_path / "marlin2-fm-recovery"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-UNDEBUG",
            "-pthread",
            f"-I{FIXTURES}",
            f"-I{FIXTURES / 'marlin2-owner-compat'}",
            f"-I{FIXTURES / 'fm-compat'}",
            f"-I{FIXTURES / 'bluetooth-compat'}",
            f"-I{KERNEL}",
            str(FIXTURES / "marlin2-fm.c"),
            str(KERNEL / "marlin2-fm.c"),
            str(KERNEL / "marlin2-fm-wire.c"),
            str(KERNEL / "marlin2-transport.c"),
            str(KERNEL / "marlin2-transport-wire.c"),
            str(KERNEL / "marlin2-packet-codec.c"),
        ],
        name="compile Marlin2 FM recovery host consumer checks",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run Marlin2 FM recovery host consumer checks",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("Marlin2 FM fresh runtime host consumer cases passed\n")
