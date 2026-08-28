# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the public check command-line interface."""

from __future__ import annotations

import re
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]
PUBLIC_COMMANDS = (
    "doctor",
    "check",
    "setup",
    "build",
    "checksum",
    "package",
    "prune",
    "run",
    "console",
    "verify",
)
PUBLIC_CHECK_SCOPES = (
    "source",
    "container",
    "metadata",
    "docs",
    "spelling",
    "secrets",
    "licenses",
    "python",
    "shell",
    "alpine",
    "c",
)


class CheckCommandTests(unittest.TestCase):
    """Exercise parsing paths that must not start Podman."""

    def run_fplinux(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run the repository entrypoint and capture its short response."""
        return run_process(
            [str(ROOT / "fplinux"), *arguments],
            name="public fplinux command",
            timeout=10,
            cwd=ROOT,
        )

    def run_check(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run the repository entrypoint and capture its short response."""
        return self.run_fplinux("check", *arguments)

    def test_help_lists_only_the_public_commands(self) -> None:
        """Keep the hook-only command out of the public command surface."""
        result = self.run_fplinux("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(
            tuple(re.findall(r"^    ([a-z][a-z-]*)\s{2,}", result.stdout, flags=re.MULTILINE)),
            PUBLIC_COMMANDS,
        )
        self.assertNotIn("_commit-msg", result.stdout)

    def test_list_wraps_every_inner_source_scope_in_host_boundaries(self) -> None:
        """Expose the inner checker scopes between repository and kernel checks."""
        result = self.run_check("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.splitlines(),
            ["repository", *PUBLIC_CHECK_SCOPES, "kernel"],
        )
        self.assertEqual(result.stderr, "")

    def test_unknown_scope_is_rejected_by_the_public_command(self) -> None:
        """Report a user-facing error for a scope outside the supported registry."""
        result = self.run_check("imaginary")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid choice: 'imaginary'", result.stderr)


if __name__ == "__main__":
    unittest.main()
