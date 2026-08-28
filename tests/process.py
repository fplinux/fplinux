# SPDX-License-Identifier: GPL-2.0-only
"""Bounded subprocess execution for host-side test tiers."""

from __future__ import annotations

import os
import signal
import subprocess
import time
from contextlib import suppress
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping, Sequence
    from pathlib import Path
    from typing import NoReturn

_TERMINATE_GRACE_SECONDS = 1.0


def _terminate_and_reap(process: subprocess.Popen[str]) -> tuple[str, str]:
    """Stop the isolated test process group and collect its final output."""
    with suppress(ProcessLookupError):
        os.killpg(process.pid, signal.SIGTERM)
    try:
        output = process.communicate(timeout=_TERMINATE_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        output = process.communicate()
    with suppress(ProcessLookupError):
        os.killpg(process.pid, signal.SIGKILL)
    return output


def _raise_timeout(command: Sequence[str], timeout: float) -> NoReturn:
    """Raise the standard timeout at one lint-friendly boundary."""
    raise subprocess.TimeoutExpired(command, timeout)


def run_process(  # noqa: PLR0913 -- the explicit process boundary stays flat.
    command: Sequence[str],
    *,
    name: str,
    timeout: float,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    while_running: Callable[[subprocess.Popen[str], float], None] | None = None,
    check: bool = False,
) -> subprocess.CompletedProcess[str]:
    """Run one isolated test process with a deadline and guaranteed reap."""
    if timeout <= 0:
        message = "test process timeout must be positive"
        raise ValueError(message)
    process = subprocess.Popen(
        list(command),
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    deadline = time.monotonic() + timeout
    try:
        if while_running is not None:
            while_running(process, deadline)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            _raise_timeout(command, timeout)
        stdout, stderr = process.communicate(timeout=remaining)
    except subprocess.TimeoutExpired as error:
        stdout, stderr = _terminate_and_reap(process)
        message = (
            f"{name} timed out after {timeout:g}s"
            f"\nstdout:\n{stdout[-4000:]}"
            f"\nstderr:\n{stderr[-4000:]}"
        )
        raise AssertionError(message) from error
    except BaseException:
        _terminate_and_reap(process)
        raise
    result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    if check and result.returncode:
        raise subprocess.CalledProcessError(
            result.returncode,
            command,
            output=stdout,
            stderr=stderr,
        )
    return result
