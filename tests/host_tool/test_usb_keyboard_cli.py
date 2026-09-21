# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral argument checks for the host USB keyboard bridge."""

from __future__ import annotations

import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "common/host/fplinux-usb-keyboard.c"
SHARED = ROOT / "include/fplinux"


class FPLinuxUsbKeyboardCliTests(unittest.TestCase):
    """Compile and exercise the real host tool without accessing USB devices."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Compile the production source with its declared host dependencies."""
        try:
            libusb_flags = shlex.split(
                run_process(
                    ["pkg-config", "--cflags", "--libs", "libusb-1.0"],
                    name="read libusb compiler and linker flags",
                    timeout=10,
                    check=True,
                ).stdout
            )
        except (FileNotFoundError, subprocess.CalledProcessError) as error:
            message = "pkg-config libusb-1.0 development files are required"
            raise unittest.SkipTest(message) from error

        cls.temporary = tempfile.TemporaryDirectory(prefix="fplinux-usb-keyboard-cli-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "fplinux-usb-keyboard"
        run_process(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(SHARED),
                str(SOURCE),
                *libusb_flags,
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                "-pthread",
                "-o",
                str(cls.executable),
            ],
            name="compile fplinux-usb-keyboard",
            timeout=30,
            check=True,
        )

    def run_keyboard(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run one bounded CLI-only or self-test invocation."""
        return run_process(
            [str(self.executable), *arguments],
            name="run fplinux-usb-keyboard argument check",
            timeout=5,
        )

    def assert_syntax_error(self, *arguments: str, message: str | None = None) -> None:
        """Require the shared stderr-only syntax-error contract."""
        result = self.run_keyboard(*arguments)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(result.stdout, "")
        if message is not None:
            self.assertIn(message, result.stderr)
        self.assertIn("--help' for more information.", result.stderr)

    def test_help_uses_the_argument_table_and_takes_priority(self) -> None:
        """Parsed help succeeds before syntax and semantic argument errors."""
        expected_help = self.run_keyboard("--help").stdout
        for arguments in (
            ("-h",),
            ("--help",),
            ("--unknown", "--help"),
            ("--vid", "not-hex", "--help"),
            ("unexpected", "--help"),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_keyboard(*arguments)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertEqual(result.stdout, expected_help)
                self.assertTrue(expected_help.startswith(f"Usage: {self.executable}"))
                self.assertIn("Forward one Linux evdev keyboard", expected_help)
                for option in (
                    "--vid",
                    "--pid",
                    "--interface",
                    "--keyboard",
                    "--bus",
                    "--address",
                    "--timeout-ms",
                    "--wait",
                    "--no-detach",
                    "--list",
                    "--self-test",
                    "--help",
                ):
                    self.assertIn(option, expected_help)

    def test_missing_value_does_not_turn_consumed_help_into_help(self) -> None:
        """An option value named --help remains that option's supplied value."""
        self.assert_syntax_error(
            "--vid",
            "--help",
            message="--vid must be a 16-bit hexadecimal value",
        )

    def test_unknown_positional_and_missing_required_arguments_are_errors(self) -> None:
        """Malformed and incomplete forwarding commands fail before device access."""
        for arguments, message in (
            (("--unknown",), None),
            (("unexpected",), None),
            (("--timeout-ms",), None),
            (("--help=yes",), None),
            ((), "--interface and --keyboard are required"),
            (("--interface", "3"), "--interface and --keyboard are required"),
            (("--keyboard", "/dev/input/event0"), "--interface and --keyboard are required"),
        ):
            with self.subTest(arguments=arguments):
                self.assert_syntax_error(*arguments, message=message)

    def test_self_test_accepts_native_value_forms_prefixes_and_repeats(self) -> None:
        """Supported numeric syntax, long prefixes and repeated options remain valid."""
        result = self.run_keyboard(
            "--v=0525",
            "--vid",
            "0x0525",
            "--p",
            "a4a6",
            "--bus=010",
            "--b",
            "0x08",
            "--address",
            "9",
            "--address=0x09",
            "--t=0xfa",
            "--timeout-ms",
            "250",
            "--w",
            "00",
            "--no-d",
            "--no-detach",
            "--self",
            "--self-test",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "SELFTEST OK\n")
        self.assertEqual(result.stderr, "")

    def test_every_supplied_numeric_occurrence_is_validated(self) -> None:
        """An invalid earlier scalar cannot be hidden by a later valid value."""
        cases = (
            (("--vid=10000",), "--vid must be a 16-bit hexadecimal value"),
            (("--pid", "-1"), "--pid must be a 16-bit hexadecimal value"),
            (("--interface", "256"), "--interface must be in 0..255"),
            (("--bus", "256", "--address", "1"), "--bus must be in 0..255"),
            (("--bus", "1", "--address=0x100"), "--address must be in 0..255"),
            (("--timeout-ms", "0"), "--timeout-ms must be in 1..60000"),
            (("--wait=3601",), "--wait must be in 0..3600"),
            (
                ("--interface", " -18446744073709551615"),
                "--interface must be in 0..255",
            ),
            (("--vid", "gg", "--vid=0525"), "--vid must be a 16-bit hexadecimal value"),
            (("--wait", "bad", "--wait", "0"), "--wait must be in 0..3600"),
        )
        for arguments, message in cases:
            with self.subTest(arguments=arguments):
                self.assert_syntax_error(*arguments, "--self-test", message=message)

    def test_self_test_keeps_non_forwarding_constraints(self) -> None:
        """Self-test bypasses required forwarding options but not combination rules."""
        result = self.run_keyboard("--self-test")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "SELFTEST OK\n")
        self.assertEqual(result.stderr, "")

        for arguments, message in (
            (("--self-test", "--bus", "1"), "--bus and --address must be used together"),
            (("--self-test", "--address", "1"), "--bus and --address must be used together"),
            (
                ("--self-test", "--interface", "1"),
                "--list and --self-test cannot be combined with forwarding options",
            ),
        ):
            with self.subTest(arguments=arguments):
                self.assert_syntax_error(*arguments, message=message)


if __name__ == "__main__":
    unittest.main()
