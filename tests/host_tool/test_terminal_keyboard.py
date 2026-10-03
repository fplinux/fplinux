# SPDX-License-Identifier: GPL-2.0-only
"""Host XKB adapter checks with a small fixture map and with real layout data."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
TERMINAL = ROOT / "alpine/aports/fplinux-terminal"
# The build image's xkeyboard-config, which libxkbcommon depends on.
SYSTEM_XKB = Path("/usr/share/X11/xkb")


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
                    str(TERMINAL),
                    str(ROOT / "tests/host_tool/fplinux-terminal-keyboard.c"),
                    str(TERMINAL / "terminal-keyboard.c"),
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


class TerminalLayoutKeyTests(unittest.TestCase):
    """Run documented keyboard keys through the terminal with real layout data.

    The data root holds the upstream xkeyboard-config files installed in the
    build image and the tracked ``fplinux(function_keys)`` symbols. It is not
    the built ``fplinux-xkb`` package, and nothing here runs on a phone.
    """

    def test_function_scrollback_and_diagnostic_keys_keep_their_actions(self) -> None:
        """F13-F24 send xterm keys; Shift+PageUp scrolls; Ctrl+Alt+F1 asks for the console."""
        with tempfile.TemporaryDirectory() as directory:
            data_root = Path(directory) / "xkb"
            for component in ("keycodes", "types", "compat", "symbols"):
                shutil.copytree(SYSTEM_XKB / component, data_root / component)
            shutil.copyfile(
                ROOT / "alpine/aports/fplinux-xkb/fplinux",
                data_root / "symbols/fplinux",
            )
            executable = Path(directory) / "terminal-layout-keys"
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
                    str(TERMINAL),
                    str(ROOT / "tests/host_tool/fplinux-terminal-shipped-keyboard.c"),
                    str(TERMINAL / "terminal-engine.c"),
                    str(TERMINAL / "terminal-input.c"),
                    str(TERMINAL / "terminal-help.c"),
                    str(TERMINAL / "terminal-keyboard.c"),
                    str(ROOT / "lib/fplinux/fplinux-keyboard-text.c"),
                    str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                    "-ltsm",
                    "-lxkbcommon",
                    "-o",
                    str(executable),
                ],
                name="compile terminal layout key harness",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable), str(data_root)],
                name="run terminal layout key harness",
                timeout=10,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
