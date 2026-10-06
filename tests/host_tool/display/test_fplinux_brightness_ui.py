# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of linked brightness controls and RGB565 render output."""

from __future__ import annotations

import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.fixtures.psf_font import write_solid_ascii_font
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

APP = ROOT / "alpine/aports/fplinux-brightness-ui"


class BrightnessUiTests:
    """Exercise real keypad decisions and rendering at both phone sizes."""

    executable: Path
    large_font: Path
    small_font: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            directory = build_directory.name
            temporary = Path(directory)
            cls.executable = temporary / "brightness-ui-host"
            cls.small_font = temporary / "small.psf"
            cls.large_font = temporary / "large.psf"
            write_solid_ascii_font(cls.small_font, width=6, height=12)
            write_solid_ascii_font(cls.large_font, width=8, height=16)
            run_process(
                [
                    "cc",
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
                    "-o",
                    str(cls.executable),
                ],
                name="compile brightness UI host behavior",
                timeout=30,
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
