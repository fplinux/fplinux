# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of linked brightness controls and RGB565 render output."""

from __future__ import annotations

from dataclasses import dataclass
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


@dataclass(frozen=True)
class BrightnessHarness:
    """Linked brightness program and its two immutable fixture fonts."""

    executable: Path
    small_font: Path
    large_font: Path


class BrightnessUiTests:
    """Exercise real keypad decisions and rendering at both phone sizes."""

    @pytest.fixture(scope="class")
    @classmethod
    def harness(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[BrightnessHarness]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "fplinux-brightness-ui") as temporary:
            executable = temporary / "brightness-ui-host"
            small_font = temporary / "small.psf"
            large_font = temporary / "large.psf"
            write_solid_ascii_font(small_font, width=6, height=12)
            write_solid_ascii_font(large_font, width=8, height=16)
            compile_c(
                executable,
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
                    str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                ],
                name="compile brightness UI host behavior",
                check=True,
            )
            yield BrightnessHarness(executable, small_font, large_font)

    def test_key_bounds_and_two_display_sizes(self, harness: BrightnessHarness) -> None:
        """Keys clamp at 0/10; both phone dimensions show ten bar segments."""
        result = run_process(
            [str(harness.executable), str(harness.small_font), str(harness.large_font)],
            name="run brightness UI host behavior",
            timeout=10,
            check=False,
        )
        assert (result.returncode) == (0), result.stderr
