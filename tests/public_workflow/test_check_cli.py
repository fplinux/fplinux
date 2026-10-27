# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the public check command-line interface."""

from __future__ import annotations

import os
import re
import shutil
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests.cli_support import prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]
PUBLIC_COMMANDS = (
    "doctor",
    "check",
    "test",
    "logs",
    "inspect",
    "format",
    "setup",
    "dependencies",
    "ci",
    "build",
    "probe-build",
    "checksum",
    "package",
    "prune",
    "run",
    "console",
    "nand",
    "device-data",
    "target",
    "verify",
)
PUBLIC_SOURCE_CHECK_SCOPES = (
    "source",
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


class CheckCommandTests:
    """Exercise parsing paths that must not start the container runtime."""

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
        assert (result.returncode) == (0), result.stderr
        assert (result.stderr) == ("")
        assert (
            tuple(re.findall(r"^    ([a-z][a-z-]*)\s{2,}", result.stdout, flags=re.MULTILINE))
        ) == (PUBLIC_COMMANDS)
        assert ("_commit-msg") not in (result.stdout)
        assert ("_pre-push") not in (result.stdout)

    def test_launcher_rejects_a_host_without_python314(self, tmp_path: Path) -> None:
        """Fail clearly before importing the CLI when its only Python is absent."""
        bash = shutil.which("bash")
        if bash is None:
            pytest.fail("bash is missing from the pinned quality image")
        empty_path = str(tmp_path)
        dirname = shutil.which("dirname")
        if dirname is None:
            pytest.fail("dirname is missing from the pinned quality image")
        (Path(empty_path) / "dirname").symlink_to(dirname)
        result = run_process(
            [bash, str(ROOT / "fplinux"), "--help"],
            name="public fplinux Python preflight",
            timeout=10,
            cwd=ROOT,
            env={**os.environ, "PATH": empty_path},
        )

        assert (result.returncode) == (1)
        assert (result.stdout) == ("")
        assert (result.stderr) == ("FPLinux requires Python 3.14.\n")

    def test_list_prints_every_scope_on_its_own_line(self) -> None:
        """Keep the machine-readable listing and include the separate test workload."""
        result = self.run_check("--list")
        assert (result.returncode) == (0), result.stderr
        assert result.stdout.splitlines() == [
            "repository",
            *PUBLIC_SOURCE_CHECK_SCOPES,
            "tests",
            "kernel",
        ]
        assert (result.stderr) == ("")

    def test_list_rejects_a_worker_limit_that_cannot_affect_listing(self) -> None:
        """Do not silently ignore a kernel execution option on the no-work path."""
        result = self.run_check("--list", "--jobs", "2")

        assert (result.returncode) != (0)
        assert ("--jobs cannot be combined with --list") in (result.stderr)

    def test_help_states_full_scope_defaults_build_types_and_kernel_worker_policy(self) -> None:
        """Help explains default workloads, build types and kernel execution limits."""
        result = self.run_check("--help")

        assert (result.returncode) == (0), result.stderr
        help_text = " ".join(result.stdout.split())
        assert "default: 3 with kernel, 1 without kernel or with --verbose" in help_text
        assert "incompatible with --jobs greater than 1" in help_text
        assert "--build-type {release,debug}" in result.stdout
        assert "Run every quality scope by default." in result.stdout
        assert re.search(r"python scope runs Python\s+analyzers", result.stdout) is not None
        assert re.search(r"tests scope runs all five test\s+tiers", result.stdout) is not None

    @pytest.mark.parametrize("value", ["0", "-1", "two"], ids=["zero", "negative", "text"])
    def test_jobs_requires_a_positive_integer_before_running_checks(self, value: str) -> None:
        """Reject malformed or nonpositive execution limits at the public parser boundary."""
        result = self.run_check("--jobs", value)

        assert (result.returncode) != (0)
        assert ("argument --jobs: must be a positive integer") in (result.stderr)

    def test_parallel_jobs_require_the_kernel_scope(self) -> None:
        """Reject an execution limit that no selected source checker can consume."""
        result = self.run_check("docs", "--jobs", "2")

        assert (result.returncode) != (0)
        assert ("--jobs greater than 1 requires the kernel check scope") in (result.stderr)

    def test_parallel_jobs_reject_verbose_streaming(self) -> None:
        """Do not advertise live streaming while parallel output is replayed in order."""
        result = self.run_check("kernel", "--jobs", "2", "--verbose")

        assert (result.returncode) != (0)
        assert ("--verbose cannot be combined with --jobs greater than 1") in (result.stderr)

    def test_unknown_scope_is_rejected_by_the_public_command(self) -> None:
        """Report a user-facing error for a scope outside the supported registry."""
        result = self.run_check("imaginary")
        assert (result.returncode) != (0)
        assert ("invalid choice: 'imaginary'") in (result.stderr)

    def test_source_rejects_resource_directory_name_before_build_runtime(
        self, tmp_path: Path
    ) -> None:
        """Check the source-wide naming policy through an isolated public checkout."""
        prepare_cli_checkout(tmp_path)
        (tmp_path / ".gitignore").write_text(".cache/\n__pycache__/\n", encoding="utf-8")
        run_process(
            ["git", "init", "--quiet", str(tmp_path)],
            name="public source check Git setup",
            timeout=10,
        )
        resource = tmp_path / "resource_names/icon.png"
        resource.parent.mkdir()
        resource.write_bytes(b"\x89PNG\x00\xff")

        result = run_process(
            [str(tmp_path / "fplinux"), "check", "source"],
            name="public source directory check",
            timeout=10,
            cwd=tmp_path,
        )

        assert result.returncode == 1
        assert "source directory must use kebab-case: resource_names" in result.stderr
