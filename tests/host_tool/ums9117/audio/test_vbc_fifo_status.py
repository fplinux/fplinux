# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for the shared VBC FIFO status decoder."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

INCLUDE = ROOT / "platforms/shared/linux/overlay/include"
COMPAT = ROOT / "tests/fixtures/ums9117-kernel/bluetooth-compat"
HARNESS = ROOT / "tests/host_tool/ums9117/audio/vbc-fifo-status.c"


def test_vbc_fifo_decoder_preserves_occupancy_and_flag_precedence(
    tmp_path: Path,
) -> None:
    """Check literal pointer, occupancy, error and contradictory-flag cases."""
    executable = tmp_path / "vbc-fifo-status"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{COMPAT}",
            f"-I{INCLUDE}",
            str(HARNESS),
        ],
        name="compile VBC FIFO status harness",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name="run VBC FIFO status harness",
        timeout=10,
        check=True,
    )
    assert result.stdout == "VBC FIFO status checks passed\n"
