# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for MicroPythonOS keyboard layout selection.

The C driver links the packaged layout and text-filter source. It does not
compile a keymap with xkbcommon, run MicroPython or open an input device.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
APORT = ROOT / "alpine/aports/fplinux-micropythonos"
SOURCE = APORT / "fplinux-keyboard-text.c"
HARNESS = ROOT / "tests/host_tool/fplinux-micropythonos-keyboard-text.c"


class MicroPythonOsKeyboardTextTests(unittest.TestCase):
    """Compose layouts from temporary data roots with literal layout files."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Compile the host driver once for this test class."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.executable = Path(cls.temporary.name) / "fplinux-micropythonos-keyboard-text"
        run_process(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(APORT),
                str(HARNESS),
                str(SOURCE),
                "-o",
                str(cls.executable),
            ],
            name="compile MicroPythonOS keyboard text driver",
            timeout=30,
            check=True,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        """Remove the compiled host driver."""
        cls.temporary.cleanup()

    def data_root(self, layouts: tuple[str, ...] | None) -> Path:
        """Create a data root whose layouts directory holds empty named files."""
        root = Path(tempfile.mkdtemp(dir=self.temporary.name))
        if layouts is not None:
            (root / "layouts").mkdir()
            for layout in layouts:
                (root / "layouts" / layout).write_bytes(b"")
        return root

    def compose(self, mode: str, layouts: tuple[str, ...] | None) -> tuple[int, str, str]:
        """Run the driver for one data root and return status, stdout and stderr."""
        result = run_process(
            [str(self.executable), mode, str(self.data_root(layouts))],
            name=f"compose MicroPythonOS keyboard {mode}",
            timeout=10,
        )
        return result.returncode, result.stdout, result.stderr

    def test_optional_layouts_follow_us_in_name_order(self) -> None:
        """Each registered layout takes the next group after US, sorted by name."""
        cases = (
            (None, "pc+us+group(alt_shift_toggle)"),
            ((), "pc+us+group(alt_shift_toggle)"),
            ((".apk.8f2c",), "pc+us+group(alt_shift_toggle)"),
            (("ru",), "pc+us+ru:2+group(alt_shift_toggle)"),
            (("ua", "ru", "by"), "pc+us+by:2+ru:3+ua:4+group(alt_shift_toggle)"),
            (("de_ch1",), "pc+us+de_ch1:2+group(alt_shift_toggle)"),
        )
        for layouts, expected in cases:
            with self.subTest(layouts=layouts):
                self.assertEqual(self.compose("symbols", layouts), (0, expected, ""))

    def test_layout_names_that_could_change_the_include_are_rejected(self) -> None:
        """Only lowercase letters, digits and '_' reach the XKB include string."""
        for name in ("Ru", "ru(phonetic)", "ru:2", "ru+us", 'ru"', "ru ua"):
            with self.subTest(name=name):
                status, output, error = self.compose("symbols", ("ru", name))

                self.assertEqual((status, output), (1, ""))
                self.assertIn("invalid keyboard layout name", error)

    def test_a_fourth_optional_layout_is_refused(self) -> None:
        """XKB has four groups, so US plus three optional layouts is the limit."""
        status, output, error = self.compose("symbols", ("by", "de", "ru", "ua"))

        self.assertEqual((status, output), (1, ""))
        self.assertIn("more than 3 optional keyboard layouts are installed", error)

    def test_control_characters_are_not_text(self) -> None:
        """Enter, Backspace, Tab, Escape, DEL and Ctrl combinations insert nothing."""
        cases = (
            ("a", True),
            ("Z", True),
            (" ", True),
            ("ж", True),
            ("€", True),
            ("\u00a0", True),
            ("", False),
            ("\r", False),
            ("\b", False),
            ("\t", False),
            ("\x1b", False),
            ("\x7f", False),
            ("\x01", False),
            ("\u0085", False),
            ("a\x1b", False),
        )
        for text, printable in cases:
            with self.subTest(text=text):
                result = run_process(
                    [str(self.executable), "printable", text],
                    name="classify MicroPythonOS keyboard text",
                    timeout=10,
                )

                self.assertEqual(result.returncode, 0 if printable else 1)


if __name__ == "__main__":
    unittest.main()
