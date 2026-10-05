# SPDX-License-Identifier: GPL-2.0-only
"""Repository hook ownership through real Git worktree queries."""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli.environment import git_hooks


class GitHookWorktreeTests(unittest.TestCase):
    """Keep selected hooks intact across linked checkouts and foreign owners."""

    def setUp(self) -> None:
        """Prepare isolated Git worktrees and their local hook configuration."""
        git = shutil.which("git")
        if git is None:
            self.skipTest("Git hook process tests require Git")
        self.git = git
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name).resolve()
        git_environment = {
            name: value for name, value in os.environ.items() if not name.startswith("GIT_")
        }
        git_environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        environment = mock.patch.dict(os.environ, git_environment, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
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

    def test_linked_checkout_preserves_registered_checkout_hooks(self) -> None:
        """An inherited absolute hook owner in this repository remains selected."""
        other = self.directory / "other linked checkout"
        self._git(self.main, "worktree", "add", "--quiet", "--detach", str(other))
        (other / ".githooks").mkdir()
        for owner in (self.main, other):
            with self.subTest(owner=owner.name):
                hooks = owner / ".githooks"
                before = self._select_hooks(hooks)
                output = io.StringIO()
                with (
                    mock.patch.object(git_hooks, "ROOT", self.linked),
                    contextlib.redirect_stdout(output),
                ):
                    git_hooks.install_git_hooks()
                self.assertEqual(self.configuration.read_bytes(), before)
                self.assertEqual(
                    self._git(self.linked, "config", "--local", "--get", "core.hooksPath"),
                    str(hooks),
                )
                self.assertEqual(output.getvalue(), f"Git hooks are ready: {hooks}\n")

    def test_foreign_hook_owners_are_rejected_without_configuration_changes(self) -> None:
        """Custom hooks and another repository's hook directory keep their owner."""
        foreign = self.directory / "foreign repository"
        foreign.mkdir()
        self._git(foreign, "init", "--quiet")
        custom = self.main / "custom-hooks"
        foreign_hooks = foreign / ".githooks"
        custom.mkdir()
        foreign_hooks.mkdir()
        for hooks in (custom, foreign_hooks):
            with self.subTest(hooks=hooks):
                before = self._select_hooks(hooks)
                with (
                    mock.patch.object(git_hooks, "ROOT", self.linked),
                    self.assertRaisesRegex(SystemExit, "core.hooksPath is already set"),
                ):
                    git_hooks.install_git_hooks()
                self.assertEqual(self.configuration.read_bytes(), before)

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
            self.assertRaisesRegex(SystemExit, "could not read the registered Git worktrees"),
        ):
            git_hooks.install_git_hooks()
        self.assertEqual(self.configuration.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
