# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the public check command-line interface."""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path

from fplinux_cli.container import CHECK_SCOPES

ROOT = Path(__file__).resolve().parents[1]


class CheckCommandTests(unittest.TestCase):
    """Exercise parsing paths that must not start Podman."""

    def run_check(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run the repository entrypoint and capture its short response."""
        return subprocess.run(
            [str(ROOT / "fplinux"), "check", *arguments],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_list_prints_scopes_in_canonical_order(self) -> None:
        """List every scope without starting a check run."""
        result = self.run_check("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), list(CHECK_SCOPES))
        self.assertEqual(result.stderr, "")

    def test_list_rejects_scope_arguments(self) -> None:
        """Reject ambiguous combinations of listing and execution arguments."""
        result = self.run_check("--list", "python")
        self.assertEqual(result.returncode, 2)
        self.assertIn("--list cannot be combined with scopes", result.stderr)

    def test_unknown_scope_is_an_argument_error(self) -> None:
        """Report unknown public scope names through argparse."""
        result = self.run_check("imaginary")
        self.assertEqual(result.returncode, 2)
        self.assertIn("invalid choice: 'imaginary'", result.stderr)


if __name__ == "__main__":
    unittest.main()
