# SPDX-License-Identifier: GPL-2.0-only
"""Run quality commands against an immutable checkout in the pinned Kern environment."""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.environment.kern import (
    kern_box_name,
    kern_environment,
)
from fplinux_cli.environment.runtime import environment_arguments

from .sanitizing import collect_reports

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.reporting.run import Stage

_QUALITY_COMMAND_TIMEOUT = 2 * 60 * 60
# Artifact tests keep temporary images beside pytest and native image-tool memory.
_QUALITY_MEMORY_MIB = 2048


def run_quality_command(  # noqa: PLR0913 -- runtime, deadline and log ownership stay explicit.
    stage: Stage,
    workspace: Path,
    command: list[str],
    *,
    kern: str,
    rootfs: Path,
    timeout: float = _QUALITY_COMMAND_TIMEOUT,
    log_name: str | None = None,
    downloads: Path | None = None,
    sanitize: bool = False,
) -> None:
    """Keep sources read-only, network disabled and full child logs in the run directory."""
    reporter = stage.reporter
    name = log_name or stage.name
    logs = reporter.root / "containers"
    logs.mkdir(parents=True, exist_ok=True)
    environment = reporter.container_environment(f"/logs/{name}")
    environment["FPLINUX_LOG_DISPLAY_ROOT"] += f"/containers/{name}"
    if sanitize:
        reports = logs / name / "sanitizer"
        reports.mkdir(parents=True, exist_ok=True)
        environment["FPLINUX_SANITIZER_LOG_ROOT"] = f"/logs/{name}/sanitizer"
        arguments = environment_arguments(rootfs)
        path = next(
            argument.removeprefix("PATH=")
            for argument in arguments
            if argument.startswith("PATH=")
        )
        environment["PATH"] = f"/workspace/tests/sanitizer:{path}"
    log_arguments = [
        part for key, value in environment.items() for part in ("--env", f"{key}={value}")
    ]
    kern_command = [
        kern,
        "box",
        kern_box_name(f"{reporter.label}-{name}"),
        *environment_arguments(rootfs),
        "--read-only",
        "--memory",
        f"{3072 if sanitize else _QUALITY_MEMORY_MIB}m",
        "--uid-range",
        "--network",
        "none",
        # Kern creates private devpts for real PTY component tests with -t.
        "-t",
        "--tmpfs",
        "/tmp:2g" if sanitize else "/tmp:1g",  # noqa: S108 -- container tmpfs.
        "--volume",
        f"{workspace}:/workspace:ro",
        *(["--volume", f"{downloads}:/cache/downloads:ro"] if downloads is not None else []),
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
    ]
    try:
        stage.run(kern_command, env=kern_environment(), timeout=timeout)
    finally:
        if sanitize:
            findings = collect_reports(reports)
            for report in findings:
                stage.write(f"\nSanitizer report: {report.relative_to(reporter.root)}\n".encode())
                stage.write(report.read_bytes())
                print(f"sanitizer finding: {report.relative_to(reporter.root)}", file=sys.stderr)
            if findings and sys.exception() is None:
                fail(f"native sanitizer reported {len(findings)} finding(s)")
