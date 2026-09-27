# SPDX-License-Identifier: GPL-2.0-only
"""Native keypad module with fake MicroPython, XKB and input-session boundaries."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]


class KeypadNativeTests(unittest.TestCase):
    """Check emitted text and releases without a real keymap or input device."""

    def test_shared_modifiers_survive_unplug_and_release_on_display_handoff(self) -> None:
        """One keyboard's Shift release cannot clear another keyboard's held Shift."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "keypad-native"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'tests/host_tool/keypad-native-compat'}",
                    f"-I{ROOT / 'include/fplinux'}",
                    f"-I{ROOT / 'alpine/aports/fplinux-micropythonos'}",
                    str(ROOT / "alpine/aports/fplinux-micropythonos/fplinux_keypad.c"),
                    str(ROOT / "tests/host_tool/fplinux-keypad-native.c"),
                    "-o",
                    str(executable),
                ],
                name="compile native keypad with fake runtime boundaries",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="observe shared keyboard modifiers and activation",
                timeout=5,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
