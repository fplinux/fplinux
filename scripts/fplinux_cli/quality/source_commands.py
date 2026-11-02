# SPDX-License-Identifier: GPL-2.0-only
"""Run source checker tools with bounded process lifetimes."""

from __future__ import annotations

import os
import shlex
import signal
import subprocess
from contextlib import suppress
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, fail
from fplinux_cli.reporting.run import current_stage

if TYPE_CHECKING:
    from pathlib import Path

_CHECK_COMMAND_TIMEOUT = 15 * 60


def _run_direct(
    command: list[str],
    *,
    timeout: float,
    capture: bool,
    env: dict[str, str] | None = None,
    cwd: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run one standalone checker tool with a finite process-group lifetime."""
    display = shlex.join(command)
    print("+", display, flush=True)
    process = subprocess.Popen(
        command,
        cwd=ROOT if cwd is None else cwd,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True,
        start_new_session=True,
        env=env,
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        fail(f"command timed out after {timeout:g}s: {display}")
    except BaseException:
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise
    return subprocess.CompletedProcess(command, process.returncode, stdout or "", stderr or "")


def run(
    command: list[str],
    *,
    timeout: float = _CHECK_COMMAND_TIMEOUT,
    env: dict[str, str] | None = None,
    cwd: Path | None = None,
) -> None:
    """Run one checker tool, using the active stage when logging is available."""
    stage = current_stage()
    if stage is not None:
        stage.run(command, cwd=ROOT if cwd is None else cwd, timeout=timeout, env=env)
        return
    result = _run_direct(command, timeout=timeout, capture=False, env=env, cwd=cwd)
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, command)


def capture(command: list[str], *, timeout: float) -> subprocess.CompletedProcess[str]:
    """Capture one checker tool while keeping the same timeout policy as stages."""
    stage = current_stage()
    if stage is None:
        return _run_direct(command, timeout=timeout, capture=True)
    result = stage.capture(command, cwd=ROOT, timeout=timeout)
    return subprocess.CompletedProcess(
        result.args,
        result.returncode,
        result.stdout.decode(errors="replace"),
        result.stderr.decode(errors="replace"),
    )
