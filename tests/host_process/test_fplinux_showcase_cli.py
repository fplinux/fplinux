# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for FPLinux Showcase command-line parsing."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
APORT = ROOT / "alpine/aports/fplinux-showcase"
SHARED = ROOT / "alpine/shared"


class FplinuxShowcaseCliTests(unittest.TestCase):
    """Run the actual binary only through its argument and early-startup boundary."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Compile one strict host binary; no framebuffer or phone is supplied."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "fplinux-showcase"
        run_process(
            [
                "cc",
                "-O2",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{APORT}",
                f"-I{SHARED}",
                str(APORT / "fplinux-showcase.c"),
                str(APORT / "armada-scene.c"),
                str(SHARED / "fplinux-fb-session.c"),
                "-o",
                str(cls.executable),
            ],
            name="compile FPLinux Showcase command-line boundary",
            timeout=30,
            check=True,
        )

    def test_invalid_arguments_fail_before_opening_phone_hardware(self) -> None:
        """Malformed run counts return usage without probing keypad or framebuffer."""
        invalid_arguments = (
            ("--runs",),
            ("--runs", "0"),
            ("--runs", "-1"),
            ("--runs", "+1"),
            ("--runs", "1ms"),
            ("--runs", "18446744073709551616"),
            ("--runs", "1", "extra"),
            ("--unknown",),
        )

        for arguments in invalid_arguments:
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments],
                    name=f"reject FPLinux Showcase arguments {arguments}",
                    timeout=5,
                )

                self.assertNotEqual(result.returncode, 0)
                self.assertIn("usage: fplinux-showcase [--runs N]", result.stderr)
                self.assertNotIn("required Nokia keypad", result.stderr)
                self.assertEqual(result.stdout, "")

    def test_positive_run_count_reaches_the_real_hardware_boundary(self) -> None:
        """A valid bounded run is accepted before the host lacks its Nokia keypad."""
        result = run_process(
            [str(self.executable), "--runs", "1"],
            name="accept one FPLinux Showcase cycle before Nokia hardware startup",
            timeout=5,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required Nokia keypad", result.stderr)
        self.assertNotIn("usage: fplinux-showcase", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
