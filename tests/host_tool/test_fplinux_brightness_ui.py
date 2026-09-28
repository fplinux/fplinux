# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of linked brightness controls and RGB565 render output."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

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
                    "-o",
                    str(executable),
                ],
                name="compile brightness UI host behavior",
                timeout=30,
                check=True,
            )
            result = run_process(
                [str(executable), str(temporary)],
                name="run brightness UI host behavior",
                timeout=10,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for geometry in ("128x160", "240x320"):
                frame = temporary / f"{geometry}.ppm"
                self.assertTrue(frame.is_file())
                self.assertGreater(frame.stat().st_size, 128 * 160 * 3)


if __name__ == "__main__":
    unittest.main()
