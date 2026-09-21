# SPDX-License-Identifier: GPL-2.0-only
"""Execute build steps with their environment and stage logs."""

from __future__ import annotations

import shlex
import subprocess
from contextlib import contextmanager
from typing import TYPE_CHECKING

from fplinux_cli.build_env import build_environment
from fplinux_cli.output import RunReporter, current_stage

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    environment: dict[str, str] | None = None,
) -> None:
    """Run one typed build step with deterministic environment variables."""
    effective_environment = build_environment()
    if environment is not None:
        effective_environment.update(environment)
    stage = current_stage()
    if stage is not None:
        stage.run(command, cwd=cwd, env=effective_environment)
        return
    print("+", " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(command, cwd=cwd, env=effective_environment, check=True)


@contextmanager
def report_stage(reporter: RunReporter | None, name: str) -> Iterator[None]:
    """Group typed build commands into one persistent stage log."""
    if reporter is None:
        yield
        return
    with reporter.stage(name):
        yield


def log_message(message: str) -> None:
    """Keep routine build details in the active stage log."""
    stage = current_stage()
    if stage is None:
        print(message)
        return
    stage.write((message + "\n").encode())
