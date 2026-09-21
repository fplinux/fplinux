# SPDX-License-Identifier: GPL-2.0-only
"""Run repository and commit-message checks."""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, fail
from fplinux_cli.environment.images import container_image_reference, load_container_lock
from fplinux_cli.environment.kern import (
    current_image_state,
    kern_box_name,
    kern_environment,
    require_kern,
)

if TYPE_CHECKING:
    from fplinux_cli.output import RunReporter

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
    container_lock = load_container_lock()
    kern = require_kern(container_lock)
    image = container_image_reference(container_lock)
    image_state = current_image_state(kern, image)
    if image_state is None:
        fail("commit hook requires the current build image; run ./fplinux setup")
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("commitlint"),
                "--image",
                image,
                "--pull",
                "never",
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
            env=kern_environment(),
            check=False,
            timeout=_COMMIT_MESSAGE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"commit message validation timed out after {_COMMIT_MESSAGE_TIMEOUT}s")
    if result.returncode:
        raise SystemExit(result.returncode)
