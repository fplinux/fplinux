# SPDX-License-Identifier: GPL-2.0-only
"""Host CLI checks; the host loop cannot verify a phone frequency measurement."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "alpine/aports/fplinux-cpuclock/fplinux-cpuclock.c"
SHARED_INCLUDE = ROOT / "include/fplinux"


class FplinuxCpuclockCliTests(unittest.TestCase):
    """Exercise argument validation and small counts using the host fallback loop."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Compile the production entry point with its host implementation."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "fplinux-cpuclock"
        run_process(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{SHARED_INCLUDE}",
                str(SOURCE),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                "-o",
                str(cls.executable),
            ],
            name="compile CPU clock host CLI",
            timeout=30,
            check=True,
        )

    def test_help_exits_before_measurement(self) -> None:
        """Help is available with missing or malformed counts and extra options."""
        for arguments in (("-h",), ("--help",), ("0", "--help"), ("--unknown", "--help")):
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments],
                    name="run CPU clock help",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Usage:", result.stdout)
                self.assertIn("iterations", result.stdout)
                self.assertEqual(result.stderr, "")
                self.assertNotIn("round 1:", result.stdout)

    def test_invalid_counts_and_extra_arguments_do_not_start_measurement(self) -> None:
        """Reject partial numbers, zero and overflow before the ARM counter cast."""
        for arguments in (
            ("",),
            ("0",),
            ("-1",),
            ("1tail",),
            ("0x",),
            ("0x10",),
            ("4294967296",),
            ("18446744073709551616",),
            ("1", "0"),
            ("1", "4294967296"),
            ("1", "1tail"),
            ("1", "1", "extra"),
            ("--", "--help"),
        ):
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments],
                    name="run CPU clock argument error",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("--help", result.stderr)

    def test_decimal_counts_select_the_requested_host_rounds(self) -> None:
        """Decimal iterations and rounds select the requested workload."""
        result = run_process(
            [str(self.executable), "16", "2"],
            name="run CPU clock host fallback",
            timeout=5,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertIn("2 rounds of 16 x 256 dependent integer additions\n", result.stdout)
        self.assertIn("round 1:", result.stdout)
        self.assertIn("round 2:", result.stdout)
        self.assertNotIn("round 3:", result.stdout)
        self.assertIn("best of 2 rounds:", result.stdout)

    def test_omitted_rounds_retain_five_measurements(self) -> None:
        """A positive signed iteration count leaves the round default unchanged."""
        result = run_process(
            [str(self.executable), "+16"],
            name="run CPU clock default host rounds",
            timeout=5,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertIn("5 rounds of 16 x 256 dependent integer additions\n", result.stdout)
        self.assertEqual(result.stdout.count("fplinux-cpuclock: round "), 5)
        self.assertIn("best of 5 rounds:", result.stdout)


if __name__ == "__main__":
    unittest.main()
