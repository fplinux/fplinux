# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: PLR0913
"""Compact progress and persistent diagnostics for long-running CLI stages."""

from __future__ import annotations

import contextvars
import io
import json
import os
import re
import selectors
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import traceback as traceback_module
from contextlib import suppress
from datetime import UTC, datetime
from pathlib import Path
from typing import IO, TYPE_CHECKING, NoReturn, Self

from .common import ROOT, display_text

if TYPE_CHECKING:
    from collections.abc import Callable
    from types import TracebackType

_LOG_ROOT = "FPLINUX_LOG_ROOT"
_LOG_DISPLAY_ROOT = "FPLINUX_LOG_DISPLAY_ROOT"
_VERBOSE = "FPLINUX_VERBOSE"
_TAIL_LINES = 40
_TAIL_BYTES = 32 * 1024
_ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
_CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f-\x9f]")
_SAFE_NAME = re.compile(r"[^a-z0-9-]+")
_RUN_METADATA_NAME = "run.json"
_RUN_METADATA_MAX_BYTES = 64 * 1024
_ACTIVE_STAGE: contextvars.ContextVar[Stage | None] = contextvars.ContextVar(
    "fplinux_active_stage",
    default=None,
)
_ACTIVE_REPORTER: contextvars.ContextVar[RunReporter | None] = contextvars.ContextVar(
    "fplinux_active_reporter",
    default=None,
)
_REPORTED_EXCEPTION: contextvars.ContextVar[BaseException | None] = contextvars.ContextVar(
    "fplinux_reported_exception",
    default=None,
)


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


def _raise_status(returncode: int) -> NoReturn:
    raise SystemExit(exit_status(returncode))


def _subprocess_pipes(process: subprocess.Popen[bytes]) -> tuple[IO[bytes], IO[bytes]]:
    if process.stdout is None or process.stderr is None:
        message = "subprocess pipes were not created"
        raise RuntimeError(message)
    return process.stdout, process.stderr


def _stop_process_group(process_group: int) -> None:
    with suppress(ProcessLookupError):
        os.killpg(process_group, signal.SIGSTOP)


def _timestamp() -> str:
    """Return a precise UTC timestamp for run metadata."""
    return datetime.now(UTC).isoformat()


def current_stage() -> Stage | None:
    """Return the stage currently collecting diagnostics in this process."""
    return _ACTIVE_STAGE.get()


def run_entrypoint(entrypoint: Callable[[], None]) -> None:
    """Suppress a second traceback after a stage already reported an error."""
    _REPORTED_EXCEPTION.set(None)
    reporter_token = _ACTIVE_REPORTER.set(None)
    try:
        entrypoint()
    except SystemExit as error:
        reporter = _ACTIVE_REPORTER.get()
        if reporter is not None:
            reporter._finish_failure()  # noqa: SLF001 -- module-level lifecycle owner.
        if isinstance(error.code, str) and _REPORTED_EXCEPTION.get() is error:
            raise SystemExit(1) from None
        raise
    except KeyboardInterrupt as error:
        reporter = _ACTIVE_REPORTER.get()
        if reporter is not None:
            reporter._finish_interrupted()  # noqa: SLF001 -- module-level lifecycle owner.
        if _REPORTED_EXCEPTION.get() is error:
            raise SystemExit(130) from None
        raise
    except BaseException as error:
        reporter = _ACTIVE_REPORTER.get()
        if reporter is not None:
            reporter._finish_failure()  # noqa: SLF001 -- module-level lifecycle owner.
        if _REPORTED_EXCEPTION.get() is error:
            raise SystemExit(1) from None
        raise
    else:
        reporter = _ACTIVE_REPORTER.get()
        if reporter is not None:
            reporter._finish_success()  # noqa: SLF001 -- module-level lifecycle owner.
    finally:
        _ACTIVE_REPORTER.reset(reporter_token)
        _REPORTED_EXCEPTION.set(None)


class RunReporter:
    """Own one command run and create ordered stage logs below it."""

    def __init__(
        self,
        label: str,
        root: Path,
        display_root: str,
        *,
        verbose: bool,
        parent_display_root: str | None = None,
    ) -> None:
        """Initialize one run rooted at an already validated directory."""
        self.label = label
        self.root = root
        self.display_root = display_root.rstrip("/")
        self.verbose = verbose
        self._sequence = 0
        self._pid = os.getpid()
        self._started_at = _timestamp()
        self._finished_at: str | None = None
        self._status = "running"
        self._stages: list[dict[str, object]] = []
        self._parent_display_root = (
            parent_display_root.rstrip("/") if parent_display_root is not None else None
        )
        self.root.mkdir(parents=True, exist_ok=False)
        self._write_metadata()
        _ACTIVE_REPORTER.set(self)

    @classmethod
    def create(cls, command: str, *, target: str | None, verbose: bool) -> Self:
        """Create a collision-resistant host-side log directory."""
        timestamp = datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ")
        run_id = f"{timestamp}-p{os.getpid()}"
        relative = Path(".cache/logs") / command
        label = command
        if target is not None:
            relative /= target
            label = f"{command} {target}"
        relative /= run_id
        root = ROOT / relative
        suffix = 1
        while root.exists():
            root = ROOT / f"{relative}-{suffix}"
            suffix += 1
        return cls(label, root, root.relative_to(ROOT).as_posix(), verbose=verbose)

    @classmethod
    def from_environment(cls, label: str, subdirectory: str) -> Self | None:
        """Join the host-created run directory from inside a container."""
        raw_root = os.environ.get(_LOG_ROOT)
        display_root = os.environ.get(_LOG_DISPLAY_ROOT)
        if raw_root is None and display_root is None:
            return None
        if raw_root is None or display_root is None:
            message = "incomplete FPLinux logging environment"
            raise SystemExit(message)
        root = Path(raw_root)
        if not root.is_absolute():
            raise SystemExit(f"{_LOG_ROOT} must be absolute")
        if not subdirectory or "/" in subdirectory or subdirectory in {".", ".."}:
            message = "invalid FPLinux log subdirectory"
            raise SystemExit(message)
        return cls(
            label,
            root / subdirectory,
            f"{display_root.rstrip('/')}/{subdirectory}",
            verbose=os.environ.get(_VERBOSE) == "1",
            parent_display_root=display_root,
        )

    @property
    def metadata_path(self) -> Path:
        """Return the local, atomically replaced metadata file for this run."""
        return self.root / _RUN_METADATA_NAME

    def container_environment(self, mounted_root: str) -> dict[str, str]:
        """Return variables that attach a container-side reporter to this run."""
        return {
            _LOG_ROOT: mounted_root,
            _LOG_DISPLAY_ROOT: self.display_root,
            _VERBOSE: "1" if self.verbose else "0",
        }

    def stage(
        self,
        name: str,
        *,
        passthrough: bool = False,
        show_tail: bool = True,
    ) -> Stage:
        """Create the next ordered stage."""
        self._sequence += 1
        normalized = _SAFE_NAME.sub("-", name.lower()).strip("-")
        if not normalized:
            message = "stage name must contain letters or digits"
            raise ValueError(message)
        log_path = self.root / f"{self._sequence:02d}-{normalized}.log"
        display_path = f"{self.display_root}/{log_path.name}"
        return Stage(
            self,
            name,
            log_path,
            display_path,
            passthrough=passthrough,
            show_tail=show_tail,
        )

    def finish(self) -> None:
        """Print the stable location of this run's complete logs."""
        self._finish_success()
        print(f"logs: {self.display_root}", file=sys.stderr, flush=True)

    def _start_stage(self, stage: Stage) -> int:
        """Publish one newly entered stage before it starts doing work."""
        if self._status != "running":
            message = "cannot start a stage after the run has finished"
            raise RuntimeError(message)
        self._stages.append(
            {
                "name": stage.name,
                "log": stage.log_path.name,
                "status": "running",
                "exit": None,
            }
        )
        self._write_metadata()
        return len(self._stages) - 1

    def _finish_stage(self, index: int, status: str, exit_code: int | None) -> None:
        """Publish the final observed state of one entered stage."""
        stage = self._stages[index]
        stage["status"] = status
        stage["exit"] = exit_code
        if status == "failed":
            self._finish_failure(write=False)
        elif status == "interrupted":
            self._finish_interrupted(write=False)
        self._write_metadata()

    def _finish_success(self) -> None:
        """Mark a naturally completed run successful exactly once."""
        if self._status != "running":
            return
        self._status = "success"
        self._finished_at = _timestamp()
        self._write_metadata()

    def _finish_failure(self, *, write: bool = True) -> None:
        """Preserve failure rather than allowing a later success to overwrite it."""
        if self._status != "running":
            return
        self._status = "failed"
        self._finished_at = _timestamp()
        if write:
            self._write_metadata()

    def _finish_interrupted(self, *, write: bool = True) -> None:
        """Record an interrupted invocation without claiming successful completion."""
        if self._status != "running":
            return
        self._status = "interrupted"
        self._finished_at = _timestamp()
        if write:
            self._write_metadata()

    def _metadata_payload(self) -> dict[str, object]:
        """Return the fixed, invocation-derived metadata shape for this run."""
        return {
            "label": self.label,
            "pid": self._pid,
            "started_at": self._started_at,
            "finished_at": self._finished_at,
            "status": self._status,
            "stages": self._stages,
            "display_root": self.display_root,
            "parent": self._parent_display_root,
        }

    def _write_metadata(self) -> None:
        """Atomically replace metadata so readers see either the old or complete new JSON."""
        encoded = (
            json.dumps(
                self._metadata_payload(),
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            ).encode()
            + b"\n"
        )
        if len(encoded) > _RUN_METADATA_MAX_BYTES:
            message = f"run metadata exceeds {_RUN_METADATA_MAX_BYTES} bytes"
            raise RuntimeError(message)
        descriptor, temporary_name = tempfile.mkstemp(
            dir=self.root,
            prefix=f".{_RUN_METADATA_NAME}.",
            suffix=".tmp",
        )
        temporary = Path(temporary_name)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(encoded)
            temporary.replace(self.metadata_path)
        finally:
            temporary.unlink(missing_ok=True)


class Stage:
    """Collect subprocess diagnostics for one named stage."""

    def __init__(
        self,
        reporter: RunReporter,
        name: str,
        log_path: Path,
        display_path: str,
        *,
        passthrough: bool,
        show_tail: bool,
    ) -> None:
        """Initialize one stage and its output policy."""
        self.reporter = reporter
        self.name = name
        self.log_path = log_path
        self.display_path = display_path
        self.passthrough = passthrough
        self.show_tail = show_tail
        self._stream: IO[bytes] | None = None
        self._token: contextvars.Token[Stage | None] | None = None
        self._metadata_index: int | None = None
        self._interrupted_exit: int | None = None

    def __enter__(self) -> Self:
        """Open the stage log and publish the active stage."""
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self._stream = self.log_path.open("wb")
        self._token = _ACTIVE_STAGE.set(self)
        self._metadata_index = self.reporter._start_stage(self)  # noqa: SLF001
        print(f"{self.reporter.label}: {self.name} ...", file=sys.stderr, flush=True)
        return self

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        """Close the stage, report its status and preserve exceptions."""
        if exception is not None and not isinstance(exception, SystemExit):
            formatted = traceback_module.format_exception(
                exception_type,
                exception,
                traceback,
            )
            self.write("".join(formatted).encode())
            _REPORTED_EXCEPTION.set(exception)
        elif isinstance(exception, SystemExit) and isinstance(exception.code, str):
            self.write((exception.code + "\n").encode())
            _REPORTED_EXCEPTION.set(exception)
        if self._stream is not None:
            self._stream.flush()
            self._stream.close()
            self._stream = None
        if self._token is not None:
            _ACTIVE_STAGE.reset(self._token)
            self._token = None
        status, exit_code = self._outcome(exception)
        if self._metadata_index is not None:
            self.reporter._finish_stage(  # noqa: SLF001 -- reporter owns its stage records.
                self._metadata_index,
                status,
                exit_code,
            )
            self._metadata_index = None
        if exception_type is None:
            print(f"{self.reporter.label}: {self.name} OK", file=sys.stderr, flush=True)
            return

        detail = "FAILED"
        if isinstance(exception, SystemExit) and isinstance(exception.code, int):
            detail = f"FAILED (exit {exception.code})"
        elif isinstance(exception, subprocess.CalledProcessError):
            detail = f"FAILED (exit {exit_status(exception.returncode)})"
        elif isinstance(exception, KeyboardInterrupt):
            detail = "INTERRUPTED"
        print(f"{self.reporter.label}: {self.name} {detail}", file=sys.stderr, flush=True)
        if self.show_tail:
            self._show_tail()
        print(f"full log: {self.display_path}", file=sys.stderr, flush=True)

    def _outcome(self, exception: BaseException | None) -> tuple[str, int | None]:
        """Classify a stage exit without confusing process signals with success."""
        if exception is None:
            return "success", 0
        if isinstance(exception, KeyboardInterrupt) or self._interrupted_exit is not None:
            return "interrupted", self._interrupted_exit or 128 + signal.SIGINT
        if isinstance(exception, subprocess.CalledProcessError):
            return "failed", exit_status(exception.returncode)
        if isinstance(exception, SystemExit) and isinstance(exception.code, int):
            return "failed", exception.code
        return "failed", None

    def write(self, data: bytes) -> None:
        """Append already-captured diagnostic bytes to this stage."""
        if self._stream is None:
            message = "stage is not active"
            raise RuntimeError(message)
        self._stream.write(data)
        self._stream.flush()

    def run(
        self,
        command: list[str],
        *,
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
        timeout: float | None = None,
    ) -> None:
        """Run a command, logging both streams and optionally teeing them."""
        result = self._run(
            command,
            cwd=cwd,
            env=env,
            capture=False,
            timeout=timeout,
        )
        if result.returncode:
            if result.returncode < 0:
                self._interrupted_exit = exit_status(result.returncode)
            _raise_status(result.returncode)

    def capture(
        self,
        command: list[str],
        *,
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
        timeout: float | None = None,
    ) -> subprocess.CompletedProcess[bytes]:
        """Run a command while retaining its separate stdout and stderr."""
        return self._run(
            command,
            cwd=cwd,
            env=env,
            capture=True,
            timeout=timeout,
        )

    def _run(
        self,
        command: list[str],
        *,
        cwd: Path | None,
        env: dict[str, str] | None,
        capture: bool,
        timeout: float | None,
    ) -> subprocess.CompletedProcess[bytes]:
        if timeout is not None and timeout <= 0:
            message = "stage command timeout must be positive"
            raise ValueError(message)
        display = [display_text(argument) for argument in command]
        timeout_seconds = timeout if timeout is not None else 0.0
        self.write(("+ " + shlex.join(display) + "\n").encode())
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
            self.write(
                (
                    "fplinux: command timed out after "
                    f"{timeout_seconds:g}s: {shlex.join(display)}\n"
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
                    self.write(chunk)
                    terminal, captured = key.data
                    if capture:
                        captured.extend(chunk)
                    if self.reporter.verbose or self.passthrough:
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

        if forwarded_signal is not None:
            self._interrupted_exit = exit_status(-forwarded_signal)
            _raise_status(-forwarded_signal)
        return subprocess.CompletedProcess(command, returncode, bytes(stdout), bytes(stderr))

    def _show_tail(self) -> None:
        try:
            data = self.log_path.read_bytes()[-_TAIL_BYTES:]
        except OSError as error:
            print(f"could not read failure log: {error}", file=sys.stderr)
            return
        lines = data.splitlines()[-_TAIL_LINES:]
        if not lines:
            return
        print(
            f"--- last {_TAIL_LINES} lines; at most {_TAIL_BYTES // 1024} KiB ---",
            file=sys.stderr,
        )
        text = b"\n".join(lines).decode(errors="replace").replace("�", "?")
        cleaned = _CONTROL.sub("?", _ANSI.sub("?", text))
        print(cleaned, file=sys.stderr)
        print("--- end ---", file=sys.stderr)
