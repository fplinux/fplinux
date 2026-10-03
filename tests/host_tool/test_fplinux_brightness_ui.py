# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of linked brightness controls and RGB565 render output."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.fixtures.psf_font import write_solid_ascii_font
from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "alpine/aports/fplinux-brightness-ui"


class BrightnessUiTests(unittest.TestCase):
    """Exercise real keypad decisions and rendering at both phone sizes."""

    def test_key_bounds_and_two_display_sizes(self) -> None:
        """Keys clamp at 0/10; both phone dimensions show ten bar segments."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            executable = temporary / "brightness-ui-host"
            small_font = temporary / "small.psf"
            large_font = temporary / "large.psf"
            write_solid_ascii_font(small_font, width=6, height=12)
            write_solid_ascii_font(large_font, width=8, height=16)
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
                    str(ROOT / "tests/host_tool/fplinux-brightness-ui.c"),
                    str(APP / "brightness-ui.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                    "-o",
                    str(executable),
                ],
                name="compile brightness UI host behavior",
                timeout=30,
                check=True,
            )
            result = run_process(
                [str(executable), str(small_font), str(large_font)],
                name="run brightness UI host behavior",
                timeout=10,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
