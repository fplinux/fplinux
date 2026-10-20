# SPDX-License-Identifier: GPL-2.0-only
"""Run quality commands against an immutable checkout in the pinned Kern environment."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.environment.kern import (
    kern_box_name,
    kern_environment,
)
from fplinux_cli.environment.runtime import environment_arguments

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.reporting.run import Stage

_QUALITY_COMMAND_TIMEOUT = 2 * 60 * 60


def run_quality_command(  # noqa: PLR0913 -- runtime, deadline and log ownership stay explicit.
    stage: Stage,
    workspace: Path,
    command: list[str],
    *,
    kern: str,
    rootfs: Path,
    timeout: float = _QUALITY_COMMAND_TIMEOUT,
    log_name: str | None = None,
) -> None:
    """Keep sources read-only, network disabled and full child logs in the run directory."""
    reporter = stage.reporter
    name = log_name or stage.name
    logs = reporter.root / "containers"
    logs.mkdir(parents=True, exist_ok=True)
    environment = reporter.container_environment(f"/logs/{name}")
    environment["FPLINUX_LOG_DISPLAY_ROOT"] += f"/containers/{name}"
    log_arguments = [
        part for key, value in environment.items() for part in ("--env", f"{key}={value}")
    ]
    stage.run(
        [
            kern,
            "box",
            kern_box_name(f"{reporter.label}-{name}"),
            *environment_arguments(rootfs),
            "--read-only",
            "--uid-range",
            "--network",
            "none",
            # Kern creates private devpts for real PTY component tests with -t.
            "-t",
            "--tmpfs",
            "/tmp:1g",  # noqa: S108 -- container tmpfs.
            "--volume",
            f"{workspace}:/workspace:ro",
            "--volume",
            f"{logs}:/logs",
            *log_arguments,
            "--env",
            "HOME=/tmp",
            "--env",
            "PYTHONPATH=/workspace/scripts",
            "--env",
            "RUFF_CACHE_DIR=/tmp/ruff",
            "--env",
            "PYTHONDONTWRITEBYTECODE=1",
            "--workdir",
            "/workspace",
            "--init",
            "--quiet",
            "--",
            *command,
        ],
        env=kern_environment(),
        timeout=timeout,
    )
