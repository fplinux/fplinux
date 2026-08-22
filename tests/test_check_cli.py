# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the public check command-line interface."""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path

import check as source_check

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

    def test_list_wraps_every_inner_source_scope_in_host_boundaries(self) -> None:
        """Expose the inner checker scopes between repository and kernel checks."""
        result = self.run_check("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.splitlines(),
            ["repository", *source_check.SOURCE_SCOPES, "kernel"],
        )
        self.assertEqual(result.stderr, "")

    def test_unknown_scope_is_rejected_by_the_public_command(self) -> None:
        """Report a user-facing error for a scope outside the supported registry."""
        result = self.run_check("imaginary")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid choice: 'imaginary'", result.stderr)


if __name__ == "__main__":
    unittest.main()
