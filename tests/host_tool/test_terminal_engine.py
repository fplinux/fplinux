# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of the linked libtsm engine, a memory surface and a real Bash PTY."""

from __future__ import annotations

import os
import struct
import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
TERMINAL = ROOT / "alpine/aports/fplinux-terminal"


class TerminalEngineTests(unittest.TestCase):
    """Run each terminal scenario in its own harness process."""

    executable: Path
    arguments: list[str]

    @classmethod
    def setUpClass(cls) -> None:
        """Link the production terminal sources and write the fixture fonts."""
        temporary = tempfile.TemporaryDirectory(prefix="fplinux-terminal-engine-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        cls.executable = directory / "terminal-engine"
        fonts = []
        for width, height in ((6, 12), (8, 16), (8, 32)):
            font = directory / f"fixture-{height}.psf"
            # Each ASCII glyph encodes its character in seven vertical pixels.
            # The harness decodes rendered text independently of the UI tables.
            characters = range(32, 127)
            header = struct.pack("<8I", 0x864AB572, 0, 32, 1, 95, height, height, width)
            bitmap = bytearray()
            for character in characters:
                bitmap.append(0x80)
                bitmap.extend(0x80 if character & (1 << bit) else 0 for bit in range(7))
                bitmap.extend(b"\0" * (height - 8))
            unicode_table = b"".join(bytes((character, 0xFF)) for character in characters)
            font.write_bytes(header + bitmap + unicode_table)
            fonts.append(str(font))
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
                str(ROOT / "tests/host_tool/fplinux-terminal-engine.c"),
                str(TERMINAL / "terminal-engine.c"),
                str(TERMINAL / "terminal-input.c"),
                str(TERMINAL / "terminal-help.c"),
                str(TERMINAL / "terminal-pty.c"),
                str(TERMINAL / "terminal-render.c"),
                str(ROOT / "lib/fplinux/fplinux-font.c"),
                str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                "-ltsm",
                "-o",
                str(cls.executable),
            ],
            name="compile terminal engine harness",
            timeout=30,
            check=True,
        )
        cls.arguments = [str(TERMINAL / "fplinux-terminal.bashrc"), *fonts]

    def run_scenario(self, scenario: str) -> None:
        """Run one scenario with a fresh working directory, HOME and history file."""
        with tempfile.TemporaryDirectory() as directory:
            environment = os.environ.copy()
            environment.update(
                HOME=directory,
                HISTFILE=str(Path(directory) / ".bash_history"),
                LC_ALL="C.UTF-8",
            )
            result = run_process(
                [str(self.executable), scenario, *self.arguments],
                name=f"run terminal engine scenario {scenario}",
                cwd=Path(directory),
                env=environment,
                timeout=30,
            )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_phone_and_keyboard_input_drive_pty_bytes_menus_and_scrollback(self) -> None:
        """Engine state and queued PTY bytes follow the documented terminal controls."""
        for scenario in (
            "grid",
            "composition",
            "pound-focus",
            "menu-softkeys",
            "menu-choosers",
            "help-input",
            "case-holds",
            "modifier-panel",
            "function-keys",
            "scrollback",
        ):
            with self.subTest(scenario=scenario):
                self.run_scenario(scenario)

    def test_memory_surface_shows_text_input_menus_and_help(self) -> None:
        """Rendering with fixture fonts keeps text, menus and help inside the surface."""
        for scenario in (
            "render-128x160",
            "render-240x320",
            "menu-128x160",
            "menu-240x320",
            "help-render",
        ):
            with self.subTest(scenario=scenario):
                self.run_scenario(scenario)

    def test_output_wait_preserves_partial_and_coalesced_prompt(self) -> None:
        """Output waits retain a prompt prefix and a complete queued prompt."""
        self.run_scenario("prompt-stream")

    def test_host_bash_keeps_line_editing_completion_and_history(self) -> None:
        """The host Bash, started through the terminal PTY, honors phone editing keys."""
        self.run_scenario("bash")


if __name__ == "__main__":
    unittest.main()
