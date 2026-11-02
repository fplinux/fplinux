# SPDX-License-Identifier: GPL-2.0-only
"""Viewport and RGB565 pixels through the shared production component."""

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_rgb565_geometry_pixels_and_inverse_mapping(tmp_path: Path) -> None:
    executable = tmp_path / "rgb565"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/ui/fplinux-rgb565.c"),
            str(ROOT / "lib/fplinux/fplinux-rgb565.c"),
        ],
        name="link RGB565 transform",
    )
    run_process([str(executable)], name="RGB565 transform", timeout=10, check=True)
