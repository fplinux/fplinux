# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of linked brightness controls and RGB565 render output."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.fixtures.psf_font import write_solid_ascii_font
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

APP = ROOT / "alpine/aports/fplinux-brightness-ui"


class BrightnessUiTests:
    """Exercise real keypad decisions and rendering at both phone sizes."""

    executable: Path
    large_font: Path
    small_font: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "fplinux-brightness-ui") as temporary:
            cls.executable = temporary / "brightness-ui-host"
            cls.small_font = temporary / "small.psf"
            cls.large_font = temporary / "large.psf"
            write_solid_ascii_font(cls.small_font, width=6, height=12)
            write_solid_ascii_font(cls.large_font, width=8, height=16)
            compile_c(
                cls.executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include/fplinux"),
                    "-I",
                    str(APP),
                    str(ROOT / "tests/host_tool/display/fplinux-brightness-ui.c"),
                    str(APP / "brightness-ui.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                ],
                name="compile brightness UI host behavior",
                check=True,
            )
            yield

    def test_key_bounds_and_two_display_sizes(self) -> None:
        """Keys clamp at 0/10; both phone dimensions show ten bar segments."""
        result = run_process(
            [str(self.executable), str(self.small_font), str(self.large_font)],
            name="run brightness UI host behavior",
            timeout=10,
            check=False,
        )
        assert (result.returncode) == (0), result.stderr
