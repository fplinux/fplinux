# SPDX-License-Identifier: GPL-2.0-only
"""Host main-loop checks with real rendering and deterministic device doubles."""

from __future__ import annotations

import shlex
import struct
import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
TERMINAL = ROOT / "alpine/aports/fplinux-terminal"


class TerminalLoopTests(unittest.TestCase):
    """Observe frames and shell input through the host terminal main loop."""

    def test_delayed_echo_and_silent_application_frames(self) -> None:
        """Commit stays visible until echo; silent input clears without local echo."""
        self._check_scenarios("echo", "no-echo")

    def test_queued_softkey_events_preserve_hold_and_tap(self) -> None:
        """A queued hold opens Modifiers; a queued tap still needs menu selection."""
        self._check_scenarios("delayed-hold", "delayed-tap")

    def _check_scenarios(self, *scenarios: str) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            executable = temporary / "terminal-loop"
            main_object = temporary / "terminal-main.o"
            font = temporary / "fixture.psf"
            header = struct.pack("<8I", 0x864AB572, 0, 32, 1, 2, 12, 12, 8)
            font.write_bytes(header + b"\x00" * 12 + b"\x80" * 12 + b" \xffa\xff")
            flags = run_process(
                ["pkg-config", "--cflags", "libdrm"],
                name="read DRM header flags",
                timeout=10,
                check=True,
            )
            compiler = [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include/fplinux"),
                "-I",
                str(TERMINAL),
                *shlex.split(flags.stdout),
            ]
            run_process(
                [
                    *compiler,
                    "-Dmain=fplinux_terminal_main",
                    "-Dpoll=fixture_poll",
                    "-Dclock_gettime=fixture_clock_gettime",
                    "-c",
                    str(TERMINAL / "fplinux-terminal.c"),
                    "-o",
                    str(main_object),
                ],
                name="compile terminal main loop",
                timeout=30,
                check=True,
            )
            run_process(
                [
                    *compiler,
                    str(ROOT / "tests/host_tool/fplinux-terminal-loop.c"),
                    str(main_object),
                    str(TERMINAL / "terminal-engine.c"),
                    str(TERMINAL / "terminal-help.c"),
                    str(TERMINAL / "terminal-input.c"),
                    str(TERMINAL / "terminal-render.c"),
                    str(TERMINAL / "terminal-keyboard.c"),
                    str(TERMINAL / "terminal-diagnostic.c"),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                    str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                    str(ROOT / "lib/fplinux/fplinux-keyboard-text.c"),
                    "-Wl,--wrap=fplinux_terminal_keyboard_open",
                    "-ltsm",
                    "-lxkbcommon",
                    "-o",
                    str(executable),
                ],
                name="link terminal loop harness",
                timeout=30,
                check=True,
            )
            for scenario in scenarios:
                with self.subTest(scenario=scenario):
                    result = run_process(
                        [
                            str(executable),
                            str(font),
                            str(ROOT / "tests/host_tool/fixtures/terminal-xkb"),
                            scenario,
                        ],
                        name=f"run terminal loop {scenario} harness",
                        timeout=10,
                        check=False,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
