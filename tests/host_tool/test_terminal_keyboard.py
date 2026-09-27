# SPDX-License-Identifier: GPL-2.0-only
"""Host XKB adapter checks using a small, explicitly defined keyboard map."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]


class TerminalKeyboardTests(unittest.TestCase):
    """Exercise keyboard text and modifiers through the linked XKB library."""

    def test_device_modifiers_symbols_repeat_and_reset(self) -> None:
        """Real XKB state remains consistent across keyboard and phone sources."""
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "terminal-keyboard"
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
                    str(ROOT / "alpine/aports/fplinux-terminal"),
                    str(ROOT / "tests/host_tool/fplinux-terminal-keyboard.c"),
                    str(ROOT / "alpine/aports/fplinux-terminal/terminal-keyboard.c"),
                    str(ROOT / "lib/fplinux/fplinux-keyboard-text.c"),
                    "-lxkbcommon",
                    "-o",
                    str(executable),
                ],
                name="compile terminal XKB harness",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable), str(ROOT / "tests/host_tool/fixtures/terminal-xkb")],
                name="run terminal XKB harness",
                timeout=10,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
