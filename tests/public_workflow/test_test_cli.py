# SPDX-License-Identifier: GPL-2.0-only
"""Public test-command argument handling without starting a runtime."""

from __future__ import annotations

import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]


class TestCommandCliTests(unittest.TestCase):
    """Help and malformed selections complete before workspace or runtime setup."""

    def test_help_explains_selection_and_execution_options(self) -> None:
        """Expose exact selection, tier discovery and runner controls."""
        result = run_process(
            [str(ROOT / "fplinux"), "test", "--help"], name="test help", timeout=10, cwd=ROOT
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for option in ("--tier", "--verbose", "--failfast"):
            self.assertIn(option, result.stdout)
        self.assertIn("module, class or method", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_invalid_or_conflicting_selection_is_a_usage_error(self) -> None:
        """Reject invalid CLI syntax instead of silently running a broader suite."""
        for arguments in (
            ("--tier", "unknown"),
            ("tests.small.test_common", "--tier", "small"),
            (".cache.private_test",),
            ("tests.small.test_common..method",),
        ):
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(ROOT / "fplinux"), "test", *arguments],
                    name="invalid test selection",
                    timeout=10,
                    cwd=ROOT,
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertNotIn("workspace", result.stderr)
                self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
