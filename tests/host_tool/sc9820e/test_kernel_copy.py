# SPDX-License-Identifier: GPL-2.0-only
"""Run the bootstrap's exact byte copy with independent host memory canaries."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_kernel_copy_preserves_canaries_source_and_exact_odd_byte_counts(tmp_path: Path) -> None:
    """Whole, partial and empty copies preserve bytes outside their declared destinations."""
    source = ROOT / "platforms/sc9820e/bootstrap/src"
    executable = tmp_path / "kernel-copy"
    compile_c(
        executable,
        [
            "-std=c99",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffreestanding",
            "-fno-builtin",
            "-I",
            str(source),
            str(ROOT / "tests/host_tool/sc9820e/kernel-copy.c"),
            str(source / "kernel-copy.c"),
        ],
        name="compile the source kernel copy with its host canary harness",
    )
    run_process(
        [str(executable)],
        name="check exact bootstrap copy extents and unchanged memory canaries",
        timeout=10,
        check=True,
    )
