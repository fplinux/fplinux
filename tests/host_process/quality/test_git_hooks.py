# SPDX-License-Identifier: GPL-2.0-only
"""Repository hook ownership through real Git worktree queries."""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import subprocess
import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.environment import git_hooks

if TYPE_CHECKING:
    from collections.abc import Iterator


class GitHookWorktreeTests:
    """Keep selected hooks intact across linked checkouts and foreign owners."""

    @pytest.fixture(autouse=True)
    def _prepare_case(self) -> Iterator[None]:
        """Prepare isolated Git worktrees and their local hook configuration."""
        with ExitStack() as cleanup:
            git = shutil.which("git")
            if git is None:
                pytest.skip("Git hook process tests require Git")
            self.git = git
            temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(temporary)
            self.directory = Path(temporary.name).resolve()
            git_environment = {
                name: value for name, value in os.environ.items() if not name.startswith("GIT_")
            }
            git_environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
            environment = mock.patch.dict(os.environ, git_environment, clear=True)
            cleanup.enter_context(environment)
            self.main = self.directory / "main checkout"
            self.main.mkdir()
            self._git(self.main, "init", "--quiet")
            self._git(
                self.main,
                "-c",
                "user.name=Hook fixture",
                "-c",
                "user.email=fixture@example.invalid",
                "-c",
                "commit.gpgSign=false",
                "commit",
                "--allow-empty",
                "--quiet",
                "--no-verify",
                "-m",
                "fixture",
            )
            self.linked = self.directory / "linked checkout"
            self._git(self.main, "worktree", "add", "--quiet", "--detach", str(self.linked))
            (self.main / ".githooks").mkdir()
            (self.linked / ".githooks").mkdir()
            self.configuration = self.main / ".git" / "config"
            yield

    def _git(self, root: Path, *arguments: str) -> str:
        return subprocess.run(
            [self.git, "-C", str(root), *arguments],
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
        ).stdout.strip()

    def _select_hooks(self, hooks: Path | str) -> bytes:
        self._git(self.main, "config", "--local", "core.hooksPath", str(hooks))
        return self.configuration.read_bytes()

    @pytest.mark.parametrize(
        "owner_name", ["main", "other"], ids=["main-checkout", "other-linked-checkout"]
    )
    def test_linked_checkout_preserves_registered_checkout_hooks(self, owner_name: str) -> None:
        """An inherited absolute hook owner in this repository remains selected."""
        other = self.directory / "other linked checkout"
        self._git(self.main, "worktree", "add", "--quiet", "--detach", str(other))
        (other / ".githooks").mkdir()
        owner = self.main if owner_name == "main" else other
        hooks = owner / ".githooks"
        before = self._select_hooks(hooks)
        output = io.StringIO()
        with (
            mock.patch.object(git_hooks, "ROOT", self.linked),
            contextlib.redirect_stdout(output),
        ):
            git_hooks.install_git_hooks()
        assert (self.configuration.read_bytes()) == (before)
        assert (self._git(self.linked, "config", "--local", "--get", "core.hooksPath")) == (
            str(hooks)
        )
        assert (output.getvalue()) == (f"Git hooks are ready: {hooks}\n")

    @pytest.mark.parametrize(
        "owner_name", ["custom", "foreign"], ids=["custom-hooks", "foreign-repository"]
    )
    def test_foreign_hook_owners_are_rejected_without_configuration_changes(
        self, owner_name: str
    ) -> None:
        """Custom hooks and another repository's hook directory keep their owner."""
        foreign = self.directory / "foreign repository"
        foreign.mkdir()
        self._git(foreign, "init", "--quiet")
        custom = self.main / "custom-hooks"
        foreign_hooks = foreign / ".githooks"
        custom.mkdir()
        foreign_hooks.mkdir()
        hooks = custom if owner_name == "custom" else foreign_hooks
        before = self._select_hooks(hooks)
        with (
            mock.patch.object(git_hooks, "ROOT", self.linked),
            pytest.raises(SystemExit, match=r"core.hooksPath is already set"),
        ):
            git_hooks.install_git_hooks()
        assert (self.configuration.read_bytes()) == (before)

    def test_worktree_query_failure_keeps_configuration_unchanged(self) -> None:
        """A failed ownership query cannot replace the inherited hook selection."""
        before = self._select_hooks(self.main / ".githooks")
        run_git = git_hooks._run_git_hook_command  # noqa: SLF001 -- external Git boundary.

        def fail_worktree_query(git: str, *arguments: str) -> subprocess.CompletedProcess[str]:
            if arguments == ("worktree", "list", "--porcelain", "-z"):
                return subprocess.CompletedProcess([git, *arguments], 1, "", "query failed")
            return run_git(git, *arguments)

        with (
            mock.patch.object(git_hooks, "ROOT", self.linked),
            mock.patch.object(git_hooks, "_run_git_hook_command", side_effect=fail_worktree_query),
            pytest.raises(SystemExit, match="could not read the registered Git worktrees"),
        ):
            git_hooks.install_git_hooks()
        assert (self.configuration.read_bytes()) == (before)
