# SPDX-License-Identifier: GPL-2.0-only
"""Multiprocess coverage for cache-lock behaviour."""

from __future__ import annotations

import contextlib
import errno
import fcntl
import io
import multiprocessing
import os
import selectors
import signal
import sys
import tempfile
import threading
import time
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.cache.lock import _LOCK_FILENAME, cache_lock

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator
    from multiprocessing.context import SpawnContext, SpawnProcess
    from multiprocessing.queues import Queue
    from multiprocessing.synchronize import Event


_CHILD_LOCK_TIMEOUT = "parent did not release child cache lock"
_CHILD_WAIT_TIMEOUT = "parent did not release waiting child"
_EXEC_LOCK_TIMEOUT = "parent did not release exec'd shared cache lock"


class _ObservedStderr(io.StringIO):
    """Signal when the real cache-lock wait diagnostic reaches stderr."""

    def __init__(self, waiting: Event) -> None:
        super().__init__()
        self.waiting = waiting

    def write(self, text: str) -> int:
        if "cache is held by" in text and "waiting" in text:
            self.waiting.set()
        return super().write(text)


@dataclass(frozen=True)
class _LockInvocation:
    """The cache-lock arguments sent to a spawned test process."""

    exclusive: bool
    command: str
    target: str | None
    profile: str | None = None


def _hold_lock(
    cache_root: str,
    invocation: _LockInvocation,
    acquired: Event,
    release: Event,
) -> None:
    """Hold one real flock until the parent lets this ordinary process exit."""
    with cache_lock(
        Path(cache_root),
        exclusive=invocation.exclusive,
        command=invocation.command,
        target=invocation.target,
        profile=invocation.profile,
    ):
        acquired.set()
        if not release.wait(10):
            raise RuntimeError(_CHILD_LOCK_TIMEOUT)


def _wait_for_lock(
    cache_root: str,
    waiting: Event,
    acquired: Event,
    release: Event,
    output: Queue[str],
) -> None:
    """Capture the wait notice emitted by an invocation that is genuinely blocked."""
    stderr = _ObservedStderr(waiting)
    with (
        contextlib.redirect_stderr(stderr),
        cache_lock(
            Path(cache_root),
            exclusive=False,
            command="package",
            target="demo-target",
        ),
    ):
        acquired.set()
        if not release.wait(10):
            raise RuntimeError(_CHILD_WAIT_TIMEOUT)
    output.put(stderr.getvalue())


def _exec_with_shared_lock(cache_root: str, acquired_path: str, release_path: str) -> None:
    """Replace a shared-lock holder with an ordinary process that keeps it open."""
    with cache_lock(
        Path(cache_root),
        exclusive=False,
        command="run",
        target="demo-target",
    ):
        os.execv(
            sys.executable,
            [
                sys.executable,
                str(ROOT / "tests/host_process/cache/fixtures/cachelock/cache_lock_exec.py"),
                acquired_path,
                release_path,
                _EXEC_LOCK_TIMEOUT,
            ],
        )


@dataclass
class CacheLockContext:
    """Own spawned lock holders and their cleanup for one isolated cache."""

    cache_root: Path
    context: SpawnContext
    cleanup: ExitStack

    def start_holder(
        self,
        *,
        exclusive: bool,
        command: str = "build",
        target: str | None = "demo-target",
        profile: str | None = None,
    ) -> tuple[SpawnProcess, Event, Event]:
        """Start a process that has acquired the requested lock."""
        acquired = self.context.Event()
        release = self.context.Event()
        process = self.context.Process(
            target=_hold_lock,
            args=(
                str(self.cache_root),
                _LockInvocation(exclusive, command, target, profile),
                acquired,
                release,
            ),
        )
        process.start()

        def release_holder() -> None:
            release.set()
            self.join_or_kill(process)

        self.cleanup.callback(release_holder)
        if not acquired.wait(5):
            release.set()
            self.join_or_kill(process)
            pytest.fail("child did not acquire its cache lock")
        return process, acquired, release

    def stop(self, process: SpawnProcess, release: Event) -> None:
        """Release and reap a test child, including from assertion cleanup."""
        release.set()
        self.join_or_kill(process)
        assert not (process.is_alive()), "child did not exit"
        assert (process.exitcode) == (0)

    @staticmethod
    def join_or_kill(process: SpawnProcess) -> None:
        """Bound a child lifetime and reap it even when a test assertion fails."""
        process.join(10)
        if process.is_alive():
            process.terminate()
            process.join(2)
        if process.is_alive():
            process.kill()
            process.join(2)

    def exclusive_lock_available(self) -> bool:
        """Return whether a separate descriptor can immediately take EX."""
        self.cache_root.mkdir(parents=True, exist_ok=True)
        descriptor = os.open(
            self.cache_root / _LOCK_FILENAME,
            os.O_RDWR | os.O_CREAT | os.O_CLOEXEC,
            0o600,
        )
        try:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as error:
                if error.errno not in {errno.EACCES, errno.EAGAIN}:
                    raise
                return False
            fcntl.flock(descriptor, fcntl.LOCK_UN)
            return True
        finally:
            os.close(descriptor)


@pytest.fixture
def cache_lock_case() -> Iterator[CacheLockContext]:
    """Reap owned lock holders before removing their disposable cache directory."""
    with ExitStack() as cleanup:
        temporary = cleanup.enter_context(tempfile.TemporaryDirectory())
        yield CacheLockContext(
            Path(temporary) / "cache", multiprocessing.get_context("spawn"), cleanup
        )


class CacheLockTests:
    """Check the cache flock only through separate operating-system processes."""

    @pytest.mark.parametrize("timeout", [0.0, 0.2])
    def test_timeout_reports_owner_without_releasing_holder(
        self, cache_lock_case: CacheLockContext, capsys: pytest.CaptureFixture[str], timeout: float
    ) -> None:
        """A busy lock times out without disturbing its actual holder."""
        owner, _, release = cache_lock_case.start_holder(exclusive=True)
        started = time.monotonic()
        with (
            pytest.raises(SystemExit) as failure,
            cache_lock(
                cache_lock_case.cache_root,
                exclusive=False,
                command="console",
                target="demo-target",
                timeout=timeout,
            ),
        ):
            pytest.fail("a timed-out waiter acquired the lock")
        assert failure.value.code == 75
        assert time.monotonic() - started < 2
        assert owner.is_alive()
        assert not cache_lock_case.exclusive_lock_available()
        assert "command=build" in capsys.readouterr().err
        cache_lock_case.stop(owner, release)

    def test_waiter_acquires_after_release_before_deadline(
        self, cache_lock_case: CacheLockContext
    ) -> None:
        """A bounded waiter continues when the holder releases in time."""
        owner, _, release = cache_lock_case.start_holder(exclusive=True)
        timer = threading.Timer(0.1, release.set)
        timer.start()
        try:
            with cache_lock(
                cache_lock_case.cache_root,
                exclusive=False,
                command="console",
                target="demo-target",
                timeout=3,
            ):
                assert not cache_lock_case.exclusive_lock_available()
        finally:
            timer.join()
            cache_lock_case.stop(owner, release)

    def test_console_timeout_stops_before_phone_access(
        self, cache_lock_case: CacheLockContext
    ) -> None:
        """A busy lock stops the public console before any phone operation."""
        owner, _, release = cache_lock_case.start_holder(exclusive=True)
        result = run_process(
            [
                str(ROOT / "fplinux"),
                "console",
                "nokia-ta1618",
                "--lock-timeout",
                "0",
                "--exec",
                "true",
            ],
            name="console with busy data lock",
            cwd=ROOT,
            env=os.environ | {"FPLINUX_DATA_DIR": str(cache_lock_case.cache_root)},
            timeout=10,
        )
        assert result.returncode == 75
        assert "command=build" in result.stderr
        assert "lock wait timed out" in result.stderr
        assert owner.is_alive()
        cache_lock_case.stop(owner, release)

    def test_interrupting_bounded_console_wait_leaves_holder_running(
        self, cache_lock_case: CacheLockContext
    ) -> None:
        """Ctrl+C interrupts the waiting CLI without contacting the phone or its holder."""
        owner, _, release = cache_lock_case.start_holder(exclusive=True)

        def interrupt_waiter(process: subprocess.Popen[str], deadline: float) -> None:
            assert process.stderr is not None
            with selectors.DefaultSelector() as selector:
                selector.register(process.stderr, selectors.EVENT_READ)
                assert selector.select(max(0, deadline - time.monotonic()))
                assert b"waiting" in os.read(process.stderr.fileno(), 4096)
            process.send_signal(signal.SIGINT)

        result = run_process(
            [
                str(ROOT / "fplinux"),
                "console",
                "nokia-ta1618",
                "--lock-timeout",
                "10",
                "--exec",
                "true",
            ],
            name="interrupt console lock wait",
            cwd=ROOT,
            env=os.environ | {"FPLINUX_DATA_DIR": str(cache_lock_case.cache_root)},
            timeout=15,
            while_running=interrupt_waiter,
        )
        assert result.returncode == 130
        assert owner.is_alive()
        assert not cache_lock_case.exclusive_lock_available()
        cache_lock_case.stop(owner, release)

    def test_shared_invocations_run_together(self, cache_lock_case: CacheLockContext) -> None:
        """Shared cache-lock modes do not block one another."""
        first, _first_acquired, first_release = cache_lock_case.start_holder(
            exclusive=False, command="run"
        )
        second: SpawnProcess | None = None
        second_release: Event | None = None
        try:
            second, _second_acquired, second_release = cache_lock_case.start_holder(
                exclusive=False,
                command="verify",
            )
        finally:
            if second is not None and second_release is not None:
                cache_lock_case.stop(second, second_release)
            cache_lock_case.stop(first, first_release)

    def test_blocked_invocation_reports_owner_then_continues(
        self, cache_lock_case: CacheLockContext
    ) -> None:
        """A shared command waits for an exclusive build and resumes after it exits."""
        owner, _owner_acquired, owner_release = cache_lock_case.start_holder(
            exclusive=True,
            profile="usb-host-lab",
        )
        waiting = cache_lock_case.context.Event()
        acquired = cache_lock_case.context.Event()
        release = cache_lock_case.context.Event()
        output = cache_lock_case.context.Queue()
        waiter = cache_lock_case.context.Process(
            target=_wait_for_lock,
            args=(str(cache_lock_case.cache_root), waiting, acquired, release, output),
        )
        waiter.start()
        try:
            assert waiting.wait(5), "waiter did not enter the cache-lock wait"
            assert not (acquired.is_set()), "waiter bypassed the exclusive lock"
            owner_release.set()
            cache_lock_case.join_or_kill(owner)
            assert not (owner.is_alive()), "owner did not exit"
            assert (owner.exitcode) == (0)
            assert acquired.wait(5), "waiter did not continue after release"
            release.set()
            cache_lock_case.join_or_kill(waiter)
            assert not (waiter.is_alive()), "waiter did not exit"
            assert (waiter.exitcode) == (0)
            rendered = output.get(timeout=5)
            assert ("command=build") in (rendered)
            assert ("target=demo-target") in (rendered)
            assert ("profile=usb-host-lab") in (rendered)
            assert (f"pid={owner.pid}") in (rendered)
            assert ("started=") in (rendered)
            assert ("cache released; continuing") in (rendered)
        finally:
            owner_release.set()
            release.set()
            cache_lock_case.join_or_kill(owner)
            cache_lock_case.join_or_kill(waiter)
            output.close()
            output.join_thread()

    @pytest.mark.parametrize(
        "exception_type",
        [None, RuntimeError, KeyboardInterrupt],
        ids=["normal-return", "runtime-error", "keyboard-interrupt"],
    )
    def test_context_releases_after_return_exception_and_interrupt(
        self, cache_lock_case: CacheLockContext, exception_type: type[BaseException] | None
    ) -> None:
        """Normal return, errors, and Ctrl-C all leave the flock available."""
        if exception_type is None:
            with cache_lock(
                cache_lock_case.cache_root,
                exclusive=True,
                command="build",
                target="demo-target",
            ):
                pass
        else:
            with (
                pytest.raises(exception_type),
                cache_lock(
                    cache_lock_case.cache_root,
                    exclusive=True,
                    command="build",
                    target="demo-target",
                ),
            ):
                raise exception_type()
        process, _acquired, child_release = cache_lock_case.start_holder(exclusive=True)
        cache_lock_case.stop(process, child_release)

    def test_shared_lock_survives_exec_until_runner_exits(
        self, cache_lock_case: CacheLockContext
    ) -> None:
        """A runner reached through exec keeps build from taking EX too early."""
        acquired_path = cache_lock_case.cache_root / "exec-acquired"
        release_path = cache_lock_case.cache_root / "exec-release"
        process = cache_lock_case.context.Process(
            target=_exec_with_shared_lock,
            args=(str(cache_lock_case.cache_root), str(acquired_path), str(release_path)),
        )
        process.start()
        try:
            deadline = time.monotonic() + 5
            while not acquired_path.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            assert acquired_path.exists(), "child did not reach exec"
            assert not (cache_lock_case.exclusive_lock_available()), (
                "exec'd process lost its shared cache lock"
            )
        finally:
            release_path.touch()
            cache_lock_case.join_or_kill(process)
        assert not (process.is_alive()), "exec'd child did not exit"
        assert (process.exitcode) == (0)
        assert cache_lock_case.exclusive_lock_available(), (
            "shared cache lock remained after runner exit"
        )
