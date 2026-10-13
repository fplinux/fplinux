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
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.environment import git_hooks

if TYPE_CHECKING:
    from collections.abc import Iterator


@dataclass(frozen=True)
class GitCheckout:
    """Disposable linked worktrees and their shared local hook configuration."""

    git: str
    directory: Path
    main: Path
    linked: Path
    configuration: Path

    def run_git(self, root: Path, *arguments: str) -> str:
        """Run Git in the selected disposable worktree with the isolated environment."""
        return subprocess.run(
            [self.git, "-C", str(root), *arguments],
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
        ).stdout.strip()

    def select_hooks(self, hooks: Path | str) -> bytes:
        """Select a hook owner and capture the shared local configuration bytes."""
        self.run_git(self.main, "config", "--local", "core.hooksPath", str(hooks))
        return self.configuration.read_bytes()


@pytest.fixture
def git_checkout() -> Iterator[GitCheckout]:
    """Own isolated Git worktrees and a host-independent Git environment."""
    with ExitStack() as cleanup:
        git = shutil.which("git")
        if git is None:
            pytest.skip("Git hook process tests require Git")
        temporary = tempfile.TemporaryDirectory()
        cleanup.enter_context(temporary)
        directory = Path(temporary.name).resolve()
        git_environment = {
            name: value for name, value in os.environ.items() if not name.startswith("GIT_")
        }
        git_environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        environment = mock.patch.dict(os.environ, git_environment, clear=True)
        cleanup.enter_context(environment)
        checkout = GitCheckout(
            git,
            directory,
            directory / "main checkout",
            directory / "linked checkout",
            directory / "main checkout/.git/config",
        )
        checkout.main.mkdir()
        checkout.run_git(checkout.main, "init", "--quiet")
        checkout.run_git(
            checkout.main,
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
        checkout.run_git(
            checkout.main, "worktree", "add", "--quiet", "--detach", str(checkout.linked)
        )
        (checkout.main / ".githooks").mkdir()
        (checkout.linked / ".githooks").mkdir()
        yield checkout


class GitHookWorktreeTests:
    """Keep selected hooks intact across linked checkouts and foreign owners."""

    @pytest.mark.parametrize(
        "owner_name", ["main", "other"], ids=["main-checkout", "other-linked-checkout"]
    )
    def test_linked_checkout_preserves_registered_checkout_hooks(
        self, git_checkout: GitCheckout, owner_name: str
    ) -> None:
        """An inherited absolute hook owner in this repository remains selected."""
        other = git_checkout.directory / "other linked checkout"
        git_checkout.run_git(
            git_checkout.main, "worktree", "add", "--quiet", "--detach", str(other)
        )
        (other / ".githooks").mkdir()
        owner = git_checkout.main if owner_name == "main" else other
        hooks = owner / ".githooks"
        before = git_checkout.select_hooks(hooks)
        output = io.StringIO()
        with (
            mock.patch.object(git_hooks, "ROOT", git_checkout.linked),
            contextlib.redirect_stdout(output),
        ):
            git_hooks.install_git_hooks()
        assert (git_checkout.configuration.read_bytes()) == (before)
        assert (
            git_checkout.run_git(
                git_checkout.linked, "config", "--local", "--get", "core.hooksPath"
            )
        ) == (str(hooks))
        assert (output.getvalue()) == (f"Git hooks are ready: {hooks}\n")

    @pytest.mark.parametrize(
        "owner_name", ["custom", "foreign"], ids=["custom-hooks", "foreign-repository"]
    )
    def test_foreign_hook_owners_are_rejected_without_configuration_changes(
        self, git_checkout: GitCheckout, owner_name: str
    ) -> None:
        """Custom hooks and another repository's hook directory keep their owner."""
        foreign = git_checkout.directory / "foreign repository"
        foreign.mkdir()
        git_checkout.run_git(foreign, "init", "--quiet")
        custom = git_checkout.main / "custom-hooks"
        foreign_hooks = foreign / ".githooks"
        custom.mkdir()
        foreign_hooks.mkdir()
        hooks = custom if owner_name == "custom" else foreign_hooks
        before = git_checkout.select_hooks(hooks)
        with (
            mock.patch.object(git_hooks, "ROOT", git_checkout.linked),
            pytest.raises(SystemExit, match=r"core.hooksPath is already set"),
        ):
            git_hooks.install_git_hooks()
        assert (git_checkout.configuration.read_bytes()) == (before)

    def test_worktree_query_failure_keeps_configuration_unchanged(
        self, git_checkout: GitCheckout
    ) -> None:
        """A failed ownership query cannot replace the inherited hook selection."""
        before = git_checkout.select_hooks(git_checkout.main / ".githooks")
        run_git = git_hooks._run_git_hook_command  # noqa: SLF001 -- external Git boundary.

        def fail_worktree_query(git: str, *arguments: str) -> subprocess.CompletedProcess[str]:
            if arguments == ("worktree", "list", "--porcelain", "-z"):
                return subprocess.CompletedProcess([git, *arguments], 1, "", "query failed")
            return run_git(git, *arguments)

        with (
            mock.patch.object(git_hooks, "ROOT", git_checkout.linked),
            mock.patch.object(git_hooks, "_run_git_hook_command", side_effect=fail_worktree_query),
            pytest.raises(SystemExit, match="could not read the registered Git worktrees"),
        ):
            git_hooks.install_git_hooks()
        assert (git_checkout.configuration.read_bytes()) == (before)
