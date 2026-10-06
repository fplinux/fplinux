# SPDX-License-Identifier: GPL-2.0-only
"""Execute subprocess groups while retaining separate output streams."""

from __future__ import annotations

import io
import os
import selectors
import shlex
import signal
import subprocess
import sys
import time
from contextlib import suppress
from dataclasses import dataclass
from typing import IO, TYPE_CHECKING, NoReturn

from fplinux_cli.common import display_text

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path


@dataclass(frozen=True)
class ProcessResult:
    """Retain completed output and a signal forwarded by the caller's process."""

    completed: subprocess.CompletedProcess[bytes]
    interrupted_exit: int | None


def silence_broken_pipe(stream: IO[str]) -> None:
    """Keep interpreter shutdown from failing after a consumer closes a pipe."""
    try:
        descriptor = stream.fileno()
    except AttributeError, OSError, ValueError, io.UnsupportedOperation:
        return

    try:
        devnull = os.open(os.devnull, os.O_WRONLY)
    except OSError:
        return
    try:
        os.dup2(devnull, descriptor)
    except OSError:
        pass
    finally:
        os.close(devnull)


def _write_terminal(stream: IO[str], data: bytes) -> None:
    try:
        buffer = getattr(stream, "buffer", None)
        if buffer is None:
            stream.write(data.decode(errors="replace"))
            stream.flush()
            return
        buffer.write(data)
        buffer.flush()
    except BrokenPipeError:
        silence_broken_pipe(stream)


def exit_status(returncode: int) -> int:
    """Convert a subprocess return code to its shell-visible status."""
    return returncode if returncode >= 0 else 128 - returncode


def _subprocess_pipes(process: subprocess.Popen[bytes]) -> tuple[IO[bytes], IO[bytes]]:
    if process.stdout is None or process.stderr is None:
        message = "subprocess pipes were not created"
        raise RuntimeError(message)
    return process.stdout, process.stderr


def _stop_process_group(process_group: int) -> None:
    with suppress(ProcessLookupError):
        os.killpg(process_group, signal.SIGSTOP)


def run_process(  # noqa: PLR0913 -- invocation inputs and output policies stay explicit.
    command: list[str],
    *,
    cwd: Path | None,
    env: dict[str, str] | None,
    capture: bool,
    timeout: float | None,
    write: Callable[[bytes], None],
    tee: bool,
) -> ProcessResult:
    """Collect and optionally tee both pipes, borrowing signal handlers until cleanup."""
    if timeout is not None and timeout <= 0:
        message = "stage command timeout must be positive"
        raise ValueError(message)
    display = [display_text(argument) for argument in command]
    timeout_seconds = timeout if timeout is not None else 0.0
    write(("+ " + shlex.join(display) + "\n").encode())
    deadline = None if timeout is None else time.monotonic() + timeout
    selector = selectors.DefaultSelector()
    termination_signals = (
        signal.SIGINT,
        signal.SIGTERM,
        signal.SIGHUP,
        signal.SIGQUIT,
    )
    suspension_signals = (signal.SIGTSTP, signal.SIGTTIN, signal.SIGTTOU)
    handled_signals = (*termination_signals, *suspension_signals, signal.SIGCONT)
    previous_handlers = {signum: signal.getsignal(signum) for signum in handled_signals}
    forwarded_signal: int | None = None
    termination_escalated = False
    process: subprocess.Popen[bytes] | None = None
    stdout = bytearray()
    stderr = bytearray()

    def expire() -> NoReturn:
        write(
            (
                f"fplinux: command timed out after {timeout_seconds:g}s: {shlex.join(display)}\n"
            ).encode()
        )
        raise subprocess.TimeoutExpired(
            command,
            timeout_seconds,
            output=bytes(stdout),
            stderr=bytes(stderr),
        )

    def forward_termination(signum: int, _frame: object) -> None:
        nonlocal forwarded_signal, termination_escalated
        if forwarded_signal is None:
            forwarded_signal = signum
        else:
            termination_escalated = True
        if process is None:
            return
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL if termination_escalated else signum)

    def forward_suspension(signum: int, _frame: object) -> None:
        if process is not None:
            _stop_process_group(process.pid)
        signal.signal(signum, signal.SIG_DFL)
        os.kill(os.getpid(), signum)
        signal.signal(signum, forward_suspension)

    def forward_continuation(signum: int, _frame: object) -> None:
        if process is not None:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signum)

    try:
        for signum in termination_signals:
            signal.signal(signum, forward_termination)
        for signum in suspension_signals:
            signal.signal(signum, forward_suspension)
        signal.signal(signal.SIGCONT, forward_continuation)
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        if forwarded_signal is not None:
            with suppress(ProcessLookupError):
                os.killpg(
                    process.pid,
                    signal.SIGKILL if termination_escalated else forwarded_signal,
                )
        process_stdout, process_stderr = _subprocess_pipes(process)
        selector.register(process_stdout, selectors.EVENT_READ, (sys.stdout, stdout))
        selector.register(process_stderr, selectors.EVENT_READ, (sys.stderr, stderr))
        while selector.get_map():
            select_timeout: float | None = None
            if deadline is not None:
                select_timeout = deadline - time.monotonic()
                if select_timeout <= 0:
                    expire()
            events = selector.select(select_timeout)
            if not events and deadline is not None:
                expire()
            for key, _events in events:
                chunk = os.read(key.fd, 64 * 1024)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                write(chunk)
                terminal, captured = key.data
                if capture:
                    captured.extend(chunk)
                if tee:
                    _write_terminal(terminal, chunk)
        if deadline is None:
            returncode = process.wait()
        else:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                expire()
            try:
                returncode = process.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                expire()
    except BaseException:
        if process is not None:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        raise
    finally:
        selector.close()
        if process is not None:
            for stream in (process.stdout, process.stderr):
                if stream is not None:
                    with suppress(OSError):
                        stream.close()
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)

    interrupted_exit = None if forwarded_signal is None else exit_status(-forwarded_signal)
    completed = subprocess.CompletedProcess(command, returncode, bytes(stdout), bytes(stderr))
    return ProcessResult(completed, interrupted_exit)
