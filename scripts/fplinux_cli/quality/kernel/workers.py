# SPDX-License-Identifier: GPL-2.0-only
"""Own concurrent kernel analyzers, cancellation and captured process output."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.reporting.process import exit_status

from .contexts import context_label

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import BinaryIO, TextIO


_CONTEXT_TERMINATE_TIMEOUT = 5.0


_CONTEXT_KILL_TIMEOUT = 5.0


@dataclass
class _ContextWorker:
    """Own one child analyzer and its process-local scratch directories."""

    index: int
    target: str
    profile: str | None
    process: subprocess.Popen[bytes]
    workspace: tempfile.TemporaryDirectory[str]
    stdout: BinaryIO
    stderr: BinaryIO


def _context_worker_command(
    target: str, profile: str | None, *, build_type: str = "release"
) -> list[str]:
    """Return the internal command that checks exactly one context."""
    command = [
        sys.executable,
        "-m",
        "fplinux_cli.quality.kernel",
        "check",
        "--jobs",
        "1",
        "--context-target",
        target,
        "--build-type",
        build_type,
    ]
    if profile is not None:
        command.extend(("--profile", profile))
    return command


def _start_context_worker(
    index: int,
    target: str,
    profile: str | None,
    command_for: Callable[[str, str | None], list[str]],
) -> _ContextWorker:
    """Start one analyzer with isolated HOME, TMPDIR, and output streams."""
    label = context_label(target, profile)
    workspace = tempfile.TemporaryDirectory(prefix=f"fplinux-kernel-{label}-")
    root = Path(workspace.name)
    home = root / "home"
    temporary = root / "tmp"
    home.mkdir()
    temporary.mkdir()
    environment = {
        **os.environ,
        "HOME": str(home),
        "TMPDIR": str(temporary),
    }
    stdout = (root / "stdout").open("w+b")
    stderr = (root / "stderr").open("w+b")
    try:
        process = subprocess.Popen(
            command_for(target, profile),
            stdout=stdout,
            stderr=stderr,
            env=environment,
        )
    except BaseException:
        stdout.close()
        stderr.close()
        workspace.cleanup()
        raise
    return _ContextWorker(
        index=index,
        target=target,
        profile=profile,
        process=process,
        workspace=workspace,
        stdout=stdout,
        stderr=stderr,
    )


def _completed_context_workers(
    running: dict[int, _ContextWorker],
) -> list[tuple[_ContextWorker, int]]:
    """Poll and reap every worker that has reached a terminal state."""
    completed: list[tuple[_ContextWorker, int]] = []
    for worker in sorted(running.values(), key=lambda item: item.index):
        returncode = worker.process.poll()
        if returncode is None:
            continue
        running.pop(worker.index)
        completed.append((worker, returncode))
    return completed


def _wait_context_workers(running: dict[int, _ContextWorker], timeout: float) -> None:
    """Reap workers that exit within one bounded cancellation phase."""
    deadline = time.monotonic() + timeout
    while running and time.monotonic() < deadline:
        _completed_context_workers(running)
        if running:
            time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))


def _stop_context_workers(running: dict[int, _ContextWorker]) -> None:
    """Escalate worker cancellation while allowing Stage to kill its tool group."""
    for worker in running.values():
        with suppress(ProcessLookupError):
            worker.process.terminate()
    _wait_context_workers(running, _CONTEXT_TERMINATE_TIMEOUT)

    # A repeated termination makes an active Stage kill its command group.
    for worker in running.values():
        with suppress(ProcessLookupError):
            worker.process.terminate()
    _wait_context_workers(running, _CONTEXT_KILL_TIMEOUT)

    for worker in running.values():
        with suppress(ProcessLookupError):
            worker.process.kill()
    _wait_context_workers(running, _CONTEXT_KILL_TIMEOUT)

    if running:
        labels = ", ".join(
            context_label(worker.target, worker.profile)
            for worker in sorted(running.values(), key=lambda item: item.index)
        )
        raise RuntimeError(f"kernel context workers could not be reaped: {labels}")


def _write_captured(stream: TextIO, data: bytes) -> None:
    """Replay child output through text streams used by the current invocation."""
    buffer = getattr(stream, "buffer", None)
    if buffer is not None:
        buffer.write(data)
        buffer.flush()
        return
    stream.write(data.decode(errors="replace"))
    stream.flush()


def _replay_context_workers(workers: list[_ContextWorker]) -> None:
    """Publish completed context output in stable target order."""
    for worker in sorted(workers, key=lambda item: item.index):
        worker.stderr.seek(0)
        _write_captured(sys.stderr, worker.stderr.read())
        worker.stdout.seek(0)
        _write_captured(sys.stdout, worker.stdout.read())


def _discard_context_workers(workers: list[_ContextWorker]) -> None:
    """Close captured streams before deleting worker scratch directories."""
    for worker in workers:
        worker.stdout.close()
        worker.stderr.close()
        worker.workspace.cleanup()


def _run_context_processes(
    contexts: tuple[tuple[str, str | None], ...],
    jobs: int,
    *,
    command_for: Callable[[str, str | None], list[str]] = _context_worker_command,
) -> None:
    """Run context workers concurrently with bounded fail-fast cancellation."""
    limit = min(jobs, len(contexts))
    workers: list[_ContextWorker] = []
    running: dict[int, _ContextWorker] = {}
    next_index = 0
    failure: tuple[int, int] | None = None

    def start_available() -> None:
        nonlocal next_index
        while next_index < len(contexts) and len(running) < limit:
            target, profile = contexts[next_index]
            worker = _start_context_worker(next_index, target, profile, command_for)
            workers.append(worker)
            running[worker.index] = worker
            next_index += 1

    try:
        start_available()
        while running:
            completed = _completed_context_workers(running)
            failed = [(worker.index, returncode) for worker, returncode in completed if returncode]
            if failed:
                failed_index, returncode = min(failed)
                failure = (failed_index, returncode)
                break
            start_available()
            if running and not completed:
                time.sleep(0.05)
        if failure is not None:
            _stop_context_workers(running)
    except BaseException:
        _stop_context_workers(running)
        raise
    finally:
        try:
            _replay_context_workers(workers)
        finally:
            _discard_context_workers(workers)

    if failure is None:
        return
    failed_index, returncode = failure
    target, profile = contexts[failed_index]
    label = context_label(target, profile)
    fail(f"kernel check failed: context {label} exited {exit_status(returncode)}")
