# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for hooks and readiness diagnostics."""

from __future__ import annotations

import contextlib
import io
import platform
import subprocess
import tempfile
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.environment import doctor as env_doctor
from fplinux_cli.environment import git_hooks


class GitHookPathTests:
    """Preserve hook ownership and bounded Git query failures."""

    def test_git_hook_timeout_is_reported_without_mutation(self) -> None:
        """A stuck Git query fails before writing repository configuration."""
        with (
            mock.patch("fplinux_cli.environment.git_hooks.shutil.which", return_value="git"),
            mock.patch(
                "fplinux_cli.environment.git_hooks.subprocess.run",
                side_effect=subprocess.TimeoutExpired(["git", "rev-parse"], 60),
            ),
            pytest.raises(SystemExit, match="Git hook configuration timed out"),
        ):
            git_hooks.install_git_hooks()

    def test_absolute_repository_hook_path_is_equivalent(self) -> None:
        """Accept the absolute spelling of this checkout's hook directory."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / ".githooks").mkdir()
            commands: list[list[str]] = []

            def fake_run(
                command: list[str], **_kwargs: object
            ) -> subprocess.CompletedProcess[str]:
                commands.append(command)
                if command[1:3] == ["rev-parse", "--show-toplevel"]:
                    return subprocess.CompletedProcess(command, 0, f"{root}\n", "")
                if command[1:5] == ["config", "--local", "--get", "core.hooksPath"]:
                    return subprocess.CompletedProcess(
                        command,
                        0,
                        f"{root / '.githooks'}\n",
                        "",
                    )
                raise AssertionError(f"unexpected Git mutation: {command}")

            with (
                mock.patch.object(git_hooks, "ROOT", root),
                mock.patch("fplinux_cli.environment.git_hooks.shutil.which", return_value="git"),
                mock.patch(
                    "fplinux_cli.environment.git_hooks.subprocess.run", side_effect=fake_run
                ),
            ):
                git_hooks.install_git_hooks()
            assert (len(commands)) == (2)

    def test_different_absolute_hook_path_is_rejected_without_mutation(self) -> None:
        """Reject another hook owner without rewriting the Git configuration."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            commands: list[list[str]] = []

            def fake_run(
                command: list[str], **_kwargs: object
            ) -> subprocess.CompletedProcess[str]:
                commands.append(command)
                if command[1:3] == ["rev-parse", "--show-toplevel"]:
                    return subprocess.CompletedProcess(command, 0, f"{root}\n", "")
                if command[1:] == ["worktree", "list", "--porcelain", "-z"]:
                    return subprocess.CompletedProcess(command, 0, f"worktree {root}\0\0", "")
                foreign = root.parent / "foreign-hooks"
                return subprocess.CompletedProcess(command, 0, f"{foreign}\n", "")

            with (
                mock.patch.object(git_hooks, "ROOT", root),
                mock.patch("fplinux_cli.environment.git_hooks.shutil.which", return_value="git"),
                mock.patch(
                    "fplinux_cli.environment.git_hooks.subprocess.run", side_effect=fake_run
                ),
                pytest.raises(SystemExit, match=r"core.hooksPath is already set"),
            ):
                git_hooks.install_git_hooks()
            assert (len(commands)) == (3)


class DoctorDiagnosticsTests:
    """Keep environment diagnostics aggregated and consistently formatted."""

    def test_missing_runtime_and_unsupported_host_are_both_reported(self) -> None:
        """A host with two problems reports both through stderr and exits once."""
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(platform, "system", return_value="Linux"),
            mock.patch.object(platform, "machine", return_value="aarch64"),
            mock.patch.object(env_doctor, "load_environment_lock", return_value={}),
            mock.patch.object(env_doctor, "kern_available", return_value=False),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
            pytest.raises(SystemExit) as raised,
        ):
            env_doctor.doctor()
        assert (raised.value.code) == (1)
        assert ("linux/amd64") in (stderr.getvalue())
        assert ("Kern is not ready") in (stderr.getvalue())
        assert all(line.startswith("fplinux: ") for line in stderr.getvalue().splitlines())
        assert ("doctor: OK") not in (stdout.getvalue())
