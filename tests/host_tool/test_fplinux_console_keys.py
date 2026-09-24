# SPDX-License-Identifier: GPL-2.0-only
"""Host component test for the local console's phone key input.

The harness links the console program as an object, feeds evdev key events with
explicit timestamps to its keypad input function and reads the shell input it
queues. It does not open a VT, a PTY or an evdev device, so the scrollback view
entered with `#` is not exercised, and it does not run on a phone.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
CONSOLE_DIRECTORY = ROOT / "alpine/aports/fplinux-console"
CONSOLE = CONSOLE_DIRECTORY / "fplinux-console.c"
MULTITAP = ROOT / "lib/fplinux/fplinux-multitap.c"
SHARED_INCLUDE = ROOT / "include/fplinux"
HARNESS = ROOT / "tests/host_tool/fplinux-console-keys.c"
COMPILE_FLAGS = ("-std=c11", "-Wall", "-Wextra", "-Werror")

# Phone key codes, written out independently of include/fplinux/fplinux-keypad.h.
DIGIT = {
    0: 0x1C4,
    1: 0x1C5,
    2: 0x1C6,
    3: 0x1C7,
    4: 0x1C8,
    5: 0x1C9,
    6: 0x1CA,
    7: 0x1CB,
    8: 0x1CC,
    9: 0x1CD,
}
STAR = 0x1CE
UP = 0x233
DOWN = 0x234
LEFT = 0x235
RIGHT = 0x236
OK = 0x237
SOFT_LEFT = 0x238
SOFT_RIGHT = 0x239
CALL = 0x23A
POWER = 0x23B


class ConsolePhoneKeyInputTests(unittest.TestCase):
    """Translate phone keypad taps into the bytes the console sends to its shell."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Link the console object, whose main is renamed, with the tap harness."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        console_object = Path(cls.temporary.name) / "fplinux-console.o"
        cls.executable = Path(cls.temporary.name) / "fplinux-console-keys"
        run_process(
            [
                "cc",
                *COMPILE_FLAGS,
                f"-I{CONSOLE_DIRECTORY}",
                f"-I{SHARED_INCLUDE}",
                "-Dmain=fplinux_console_program_main",
                "-c",
                str(CONSOLE),
                "-o",
                str(console_object),
            ],
            name="compile console object",
            timeout=30,
            check=True,
        )
        run_process(
            [
                "cc",
                *COMPILE_FLAGS,
                f"-I{CONSOLE_DIRECTORY}",
                f"-I{SHARED_INCLUDE}",
                str(HARNESS),
                str(console_object),
                str(MULTITAP),
                "-o",
                str(cls.executable),
            ],
            name="link console key harness",
            timeout=30,
            check=True,
        )

    def shell_input(self, *taps: tuple[int, int]) -> bytes:
        """Return the shell input queued by (code, milliseconds) taps in one console."""
        result = run_process(
            [str(self.executable), *(f"{code:#x}@{ms}" for code, ms in taps)],
            name="run console key harness",
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return bytes.fromhex(result.stdout.strip())

    def test_digit_keys_enter_the_first_character_of_their_group(self) -> None:
        """Each digit key starts its own character; a different key commits the previous."""
        taps = [(DIGIT[digit], 100 * digit) for digit in range(10)]

        self.assertEqual(self.shell_input(*taps, (OK, 1000)), b" .adgjmptw\r")

    def test_repeated_digit_cycles_only_within_the_timeout(self) -> None:
        """A repeat 100 ms later cycles; one 800 ms later starts a new character."""
        cases = {
            "cycle": ([(DIGIT[2], 0), (DIGIT[2], 100), (OK, 200)], b"b\r"),
            "timeout": ([(DIGIT[2], 0), (DIGIT[2], 800), (OK, 900)], b"aa\r"),
        }
        for name, (taps, expected) in cases.items():
            with self.subTest(name):
                self.assertEqual(self.shell_input(*taps), expected)

    def test_star_arms_a_one_shot_modifier(self) -> None:
        """Star commits a pending character, then cycles Ctrl, Alt, Shift and none."""
        cases = {
            "ctrl": ([(STAR, 0), (DIGIT[2], 10), (OK, 20)], b"\x01\r"),
            "alt": ([(STAR, 0), (STAR, 10), (DIGIT[2], 20), (OK, 30)], b"\x1ba\r"),
            "shift": (
                [(STAR, 0), (STAR, 10), (STAR, 20), (DIGIT[2], 30), (OK, 40)],
                b"A\r",
            ),
            "none": (
                [(STAR, 0), (STAR, 10), (STAR, 20), (STAR, 30), (DIGIT[2], 40), (OK, 50)],
                b"a\r",
            ),
            "one shot": ([(STAR, 0), (DIGIT[2], 10), (DIGIT[3], 20), (OK, 30)], b"\x01d\r"),
            "commits first": ([(DIGIT[2], 0), (STAR, 10), (DIGIT[3], 20), (OK, 30)], b"a\x04\r"),
        }
        for name, (taps, expected) in cases.items():
            with self.subTest(name):
                self.assertEqual(self.shell_input(*taps), expected)

    def test_ok_and_call_keys_send_enter(self) -> None:
        """The centre and green keys both commit a pending character and send CR."""
        cases = {
            "ok": ([(OK, 0)], b"\r"),
            "call": ([(CALL, 0)], b"\r"),
            "call after digit": ([(DIGIT[2], 0), (CALL, 10)], b"a\r"),
        }
        for name, (taps, expected) in cases.items():
            with self.subTest(name):
                self.assertEqual(self.shell_input(*taps), expected)

    def test_navigation_keys_send_linux_cursor_sequences(self) -> None:
        """The D-pad sends VT cursor keys after committing a pending character."""
        cases = {
            "up": ([(UP, 0)], b"\x1b[A"),
            "down": ([(DOWN, 0)], b"\x1b[B"),
            "right": ([(RIGHT, 0)], b"\x1b[C"),
            "left": ([(LEFT, 0)], b"\x1b[D"),
            "left after digit": ([(DIGIT[2], 0), (LEFT, 10)], b"a\x1b[D"),
        }
        for name, (taps, expected) in cases.items():
            with self.subTest(name):
                self.assertEqual(self.shell_input(*taps), expected)

    def test_right_soft_key_cancels_pending_character_or_deletes(self) -> None:
        """The right soft key drops a pending character, otherwise sends DEL."""
        cases = {
            "delete": ([(SOFT_RIGHT, 0)], b"\x7f"),
            "cancel": ([(DIGIT[2], 0), (SOFT_RIGHT, 10), (OK, 20)], b"\r"),
            "cancel then delete": (
                [(DIGIT[2], 0), (SOFT_RIGHT, 10), (SOFT_RIGHT, 20)],
                b"\x7f",
            ),
        }
        for name, (taps, expected) in cases.items():
            with self.subTest(name):
                self.assertEqual(self.shell_input(*taps), expected)

    def test_left_soft_key_sends_tab(self) -> None:
        """The left soft key sends Tab, ESC Tab with Shift and nothing with Ctrl."""
        cases = {
            "tab": ([(SOFT_LEFT, 0)], b"\t"),
            "tab after digit": ([(DIGIT[2], 0), (SOFT_LEFT, 10)], b"a\t"),
            "shift": ([(STAR, 0), (STAR, 10), (STAR, 20), (SOFT_LEFT, 30)], b"\x1b\t"),
            "ctrl": ([(STAR, 0), (SOFT_LEFT, 10)], b""),
        }
        for name, (taps, expected) in cases.items():
            with self.subTest(name):
                self.assertEqual(self.shell_input(*taps), expected)

    def test_power_and_keyboard_codes_are_not_console_input(self) -> None:
        """The power key and keyboard codes neither type nor disturb a pending character."""
        key_enter = 0x1C
        key_2 = 0x03
        cases = {
            "power": ([(POWER, 0)], b""),
            "keyboard codes": ([(key_enter, 0), (key_2, 10)], b""),
            "power while cycling": (
                [(DIGIT[2], 0), (POWER, 50), (DIGIT[2], 100), (OK, 150)],
                b"b\r",
            ),
        }
        for name, (taps, expected) in cases.items():
            with self.subTest(name):
                self.assertEqual(self.shell_input(*taps), expected)


if __name__ == "__main__":
    unittest.main()
