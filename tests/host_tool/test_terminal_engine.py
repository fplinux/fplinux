# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks with the linked libtsm engine and a real Bash PTY."""

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
    """Keep terminal state and real shell editing consistent."""

    def test_screen_input_history_and_readline(self) -> None:
        """Engine state and PTY bytes preserve the terminal editing contract."""
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "terminal-engine"
            fonts = []
            for width, height in ((6, 12), (8, 16), (8, 32)):
                font = Path(directory) / f"fixture-{height}.psf"
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
                    str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                    "-ltsm",
                    "-o",
                    str(executable),
                ],
                name="compile terminal engine harness",
                timeout=30,
                check=True,
            )
            environment = os.environ.copy()
            environment.update(HOME=directory, LC_ALL="C.UTF-8")
            result = run_process(
                [str(executable), str(TERMINAL / "fplinux-terminal.bashrc"), *fonts],
                name="run terminal engine and Bash PTY harness",
                cwd=Path(directory),
                env=environment,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
