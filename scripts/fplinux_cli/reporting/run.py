# SPDX-License-Identifier: GPL-2.0-only
"""Own command runs, stage progress and persistent diagnostics."""

from __future__ import annotations

import contextvars
import json
import os
import re
import signal
import subprocess
import sys
import traceback as traceback_module
from datetime import UTC, datetime
from pathlib import Path
from typing import IO, TYPE_CHECKING, NoReturn, Self

from fplinux_cli.common import ROOT, error_message, fail, replace_file_atomically

from .process import exit_status, run_process

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


def _raise_status(returncode: int) -> NoReturn:
    raise SystemExit(exit_status(returncode))


def _timestamp() -> str:
    """Return a precise UTC timestamp for run metadata."""
    return datetime.now(UTC).isoformat()


def current_stage() -> Stage | None:
    """Return the stage currently collecting diagnostics in this process."""
    return _ACTIVE_STAGE.get()


def _expected_error_text(error: BaseException | None) -> str | None:
    """Return a concise cause for normal failures, not programming exceptions."""
    if isinstance(error, SystemExit) and isinstance(error.code, str):
        return error.code
    if isinstance(error, KeyboardInterrupt):
        return "interrupted"
    if isinstance(error, (OSError, subprocess.SubprocessError)):
        return error_message(error)
    return None


def run_entrypoint(entrypoint: Callable[[], None]) -> None:
    """Report expected failures once and retain unexpected exception diagnostics."""
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
    except KeyboardInterrupt:
        reporter = _ACTIVE_REPORTER.get()
        if reporter is not None:
            reporter._finish_interrupted()  # noqa: SLF001 -- module-level lifecycle owner.
        raise SystemExit(130) from None
    except BaseException as error:
        reporter = _ACTIVE_REPORTER.get()
        if reporter is not None:
            reporter._finish_failure()  # noqa: SLF001 -- module-level lifecycle owner.
        if _REPORTED_EXCEPTION.get() is error:
            raise SystemExit(1) from None
        if isinstance(error, (OSError, subprocess.SubprocessError)):
            fail(str(error))
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
        replace_file_atomically(self.metadata_path, encoded, 0o600, sync=False)


class Stage:
    """Collect subprocess diagnostics for one named stage."""

    def __init__(  # noqa: PLR0913 -- log paths and output policies stay explicit.
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
        self._reporter_token: contextvars.Token[RunReporter | None] | None = None
        self._metadata_index: int | None = None
        self._interrupted_exit: int | None = None

    def __enter__(self) -> Self:
        """Open the stage log and publish the active stage."""
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self._stream = self.log_path.open("wb")
        self._token = _ACTIVE_STAGE.set(self)
        self._reporter_token = _ACTIVE_REPORTER.set(self.reporter)
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
        already_reported = exception is not None and _REPORTED_EXCEPTION.get() is exception
        cause = _expected_error_text(exception)
        if cause is not None:
            self.write((cause + "\n").encode())
            _REPORTED_EXCEPTION.set(exception)
        elif exception is not None and not isinstance(exception, SystemExit):
            formatted = traceback_module.format_exception(
                exception_type,
                exception,
                traceback,
            )
            self.write("".join(formatted).encode())
            _REPORTED_EXCEPTION.set(exception)
        if self._stream is not None:
            self._stream.flush()
            self._stream.close()
            self._stream = None
        if self._token is not None:
            _ACTIVE_STAGE.reset(self._token)
            self._token = None
        if self._reporter_token is not None:
            _ACTIVE_REPORTER.reset(self._reporter_token)
            self._reporter_token = None
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
        elif (
            cause is not None
            and not already_reported
            and not isinstance(exception, KeyboardInterrupt)
        ):
            print(cause, file=sys.stderr, flush=True)
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
        result = run_process(
            command,
            cwd=cwd,
            env=env,
            capture=capture,
            timeout=timeout,
            write=self.write,
            tee=self.reporter.verbose or self.passthrough,
        )
        if result.interrupted_exit is not None:
            self._interrupted_exit = result.interrupted_exit
            raise SystemExit(result.interrupted_exit)
        return result.completed

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
