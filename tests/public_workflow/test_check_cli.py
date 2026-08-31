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
    "format",
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

    def test_list_rejects_a_worker_limit_that_cannot_affect_listing(self) -> None:
        """Do not silently ignore a kernel execution option on the no-work path."""
        result = self.run_check("--list", "--jobs", "2")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--jobs cannot be combined with --list", result.stderr)

    def test_help_describes_the_measured_kernel_worker_default(self) -> None:
        """Expose the default and verbose fallback chosen by the public A/B."""
        result = self.run_check("--help")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout, r"default: 3, or\s+1 with --verbose")

    def test_jobs_requires_a_positive_integer_before_running_checks(self) -> None:
        """Reject malformed or nonpositive execution limits at the public parser boundary."""
        for value in ("0", "-1", "two"):
            with self.subTest(value=value):
                result = self.run_check("--jobs", value)

                self.assertNotEqual(result.returncode, 0)
                self.assertIn("argument --jobs: must be a positive integer", result.stderr)

    def test_parallel_jobs_require_the_kernel_scope(self) -> None:
        """Reject an execution limit that no selected source checker can consume."""
        result = self.run_check("docs", "--jobs", "2")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--jobs greater than 1 requires the kernel check scope", result.stderr)

    def test_parallel_jobs_reject_verbose_streaming(self) -> None:
        """Do not advertise live streaming while parallel output is replayed in order."""
        result = self.run_check("kernel", "--jobs", "2", "--verbose")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "--verbose cannot be combined with --jobs greater than 1",
            result.stderr,
        )

    def test_unknown_scope_is_rejected_by_the_public_command(self) -> None:
        """Report a user-facing error for a scope outside the supported registry."""
        result = self.run_check("imaginary")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid choice: 'imaginary'", result.stderr)


if __name__ == "__main__":
    unittest.main()
