# SPDX-License-Identifier: GPL-2.0-only
"""Exercise brightness key bounds and actual RGB565 output."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.host_tool.display.status_resources import generate
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_brightness_controls_and_screen(tmp_path: Path, ui_host_inputs: Path) -> None:
    fonts = generate(tmp_path, ui_host_inputs)
    executable = tmp_path / "brightness-ui"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            f"-I{ROOT / 'alpine/aports/fplinux-brightness-ui'}",
            str(ROOT / "tests/host_tool/display/fplinux-brightness-ui.c"),
            str(ROOT / "alpine/aports/fplinux-brightness-ui/brightness-ui.c"),
            str(ROOT / "lib/fplinux/fplinux-status-screen.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
            str(fonts),
        ],
        name="compile brightness UI host behavior",
    )
    run_process([str(executable)], name="test brightness keys and frames", timeout=10, check=True)
