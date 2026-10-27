# SPDX-License-Identifier: GPL-2.0-only
"""Run repository and commit-message checks."""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, fail
from fplinux_cli.data_paths import data_root
from fplinux_cli.environment.definition import environment_recipe_digest, load_environment_lock
from fplinux_cli.environment.kern import kern_box_name, kern_environment, require_kern
from fplinux_cli.environment.runtime import (
    current_environment_state,
    environment_arguments,
    environment_rootfs,
)

if TYPE_CHECKING:
    from fplinux_cli.reporting.run import RunReporter

_CHECK_GIT_TIMEOUT = 5 * 60


_COMMIT_MESSAGE_TIMEOUT = 5 * 60


def check_git_diff(reporter: RunReporter) -> None:
    try:
        head = subprocess.run(
            ["git", "rev-parse", "--verify", "HEAD"],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=_CHECK_GIT_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Git HEAD lookup timed out after {_CHECK_GIT_TIMEOUT}s")
    if head.returncode == 0:
        with reporter.stage("git-diff") as stage:
            stage.run(
                ["git", "diff", "--check", "HEAD", "--"],
                cwd=ROOT,
                timeout=_CHECK_GIT_TIMEOUT,
            )


def check_commit_message(message_file: str) -> None:
    """Validate one Git commit message in the pinned environment."""
    message = Path(message_file)
    if message.is_symlink() or not message.is_file():
        fail(f"commit message file is missing or invalid: {message}")
    config = ROOT / "commitlint.config.mjs"
    if config.is_symlink() or not config.is_file():
        fail("commitlint configuration is missing or invalid")
    environment_lock = load_environment_lock()
    cache = data_root(ROOT)
    kern = require_kern(environment_lock, cache=cache)
    recipe = environment_recipe_digest(environment_lock)
    environment_state = current_environment_state(kern, recipe, cache=cache)
    if environment_state is None:
        fail("commit hook requires the current build environment; run ./fplinux setup")
    rootfs = environment_rootfs(cache, environment_state)
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("commitlint"),
                *environment_arguments(rootfs),
                "--read-only",
                "--network",
                "none",
                "--tmpfs",
                "/tmp:64m",  # noqa: S108 -- container tmpfs.
                "--no-uid-range",
                "--volume",
                f"{config}:/workspace/commitlint.config.mjs:ro",
                "--volume",
                f"{message.resolve()}:/message:ro",
                "--env",
                "HOME=/tmp",
                "--workdir",
                "/workspace",
                "--quiet",
                "--",
                "sh",
                "-c",
                "commitlint < /message",
            ],
            cwd=ROOT,
            env=kern_environment(cache=cache),
            check=False,
            timeout=_COMMIT_MESSAGE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"commit message validation timed out after {_COMMIT_MESSAGE_TIMEOUT}s")
    if result.returncode:
        raise SystemExit(result.returncode)
