# SPDX-License-Identifier: GPL-2.0-only
"""Configure the repository-owned Git hooks."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from fplinux_cli.common import ROOT, fail

GIT_HOOKS_PATH = ".githooks"


_GIT_HOOK_TIMEOUT = 60


def _run_git_hook_command(git: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    """Run one bounded Git query or mutation for hook ownership."""
    try:
        return subprocess.run(
            [git, *arguments],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            timeout=_GIT_HOOK_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Git hook configuration timed out after {_GIT_HOOK_TIMEOUT}s")


def install_git_hooks() -> None:
    """Select the repository-owned hooks for this Git checkout."""
    git = shutil.which("git")
    if git is None:
        return
    checkout = _run_git_hook_command(git, "rev-parse", "--show-toplevel")
    if checkout.returncode:
        return
    if Path(checkout.stdout.strip()).resolve() != ROOT:
        fail("Git reports a different repository root")
    configured = _run_git_hook_command(
        git,
        "config",
        "--local",
        "--get",
        "core.hooksPath",
    )
    if configured.returncode not in {0, 1}:
        fail("could not read the local Git hooks path")
    hooks_path = configured.stdout.strip()
    if hooks_path:
        configured_path = Path(hooks_path)
        if not configured_path.is_absolute():
            configured_path = ROOT / configured_path
        if configured_path.resolve() != (ROOT / GIT_HOOKS_PATH).resolve():
            fail(f"core.hooksPath is already set to {hooks_path}")
    if not hooks_path:
        updated = _run_git_hook_command(
            git,
            "config",
            "--local",
            "core.hooksPath",
            GIT_HOOKS_PATH,
        )
        if updated.returncode:
            fail("could not configure the local Git hooks path")
    print(f"Git hooks are ready: {GIT_HOOKS_PATH}")
