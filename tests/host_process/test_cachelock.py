# SPDX-License-Identifier: GPL-2.0-only
"""Multiprocess coverage for cache-lock behaviour."""

from __future__ import annotations

import contextlib
import errno
import fcntl
import io
import multiprocessing
import os
import sys
import tempfile
import time
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.cachelock import _LOCK_FILENAME, cache_lock

if TYPE_CHECKING:
    from multiprocessing.context import SpawnProcess
    from multiprocessing.queues import Queue
    from multiprocessing.synchronize import Event


_CHILD_LOCK_TIMEOUT = "parent did not release child cache lock"
_CHILD_WAIT_TIMEOUT = "parent did not release waiting child"
_EXEC_LOCK_TIMEOUT = "parent did not release exec'd shared cache lock"


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
    blocked: Event,
    acquired: Event,
    release: Event,
    output: Queue[str],
) -> None:
    """Capture the wait notice emitted by an invocation that is genuinely blocked."""
    lock_path = Path(cache_root) / _LOCK_FILENAME
    descriptor = os.open(lock_path, os.O_RDWR | os.O_CLOEXEC)
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno not in {errno.EACCES, errno.EAGAIN}:
                raise
            blocked.set()
        else:
            message = "waiter unexpectedly acquired the held cache lock"
            raise RuntimeError(message)
    finally:
        os.close(descriptor)
    stderr = io.StringIO()
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
        program = (
            "from pathlib import Path\n"
            "import sys\n"
            "import time\n"
            "acquired = Path(sys.argv[1])\n"
            "release = Path(sys.argv[2])\n"
            "acquired.touch()\n"
            "deadline = time.monotonic() + 10\n"
            "while not release.exists():\n"
            "    if time.monotonic() >= deadline:\n"
            "        raise RuntimeError(sys.argv[3])\n"
            "    time.sleep(0.01)\n"
        )
        os.execv(
            sys.executable,
            [
                sys.executable,
                "-c",
                program,
                acquired_path,
                release_path,
                _EXEC_LOCK_TIMEOUT,
            ],
        )


class CacheLockTests(unittest.TestCase):
    """Check the cache flock only through separate operating-system processes."""

    def setUp(self) -> None:
        """Create a disposable cache directory and spawned-process context."""
        self.temporary = tempfile.TemporaryDirectory()
        self.cache_root = Path(self.temporary.name) / "cache"
        self.context = multiprocessing.get_context("spawn")

    def tearDown(self) -> None:
        """Discard the test directory once every child has stopped."""
        self.temporary.cleanup()

    def _start_holder(
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
        if not acquired.wait(5):
            release.set()
            self._join_or_kill(process)
            self.fail("child did not acquire its cache lock")
        return process, acquired, release

    def _stop(self, process: SpawnProcess, release: Event) -> None:
        """Release and reap a test child, including from assertion cleanup."""
        release.set()
        self._join_or_kill(process)
        self.assertFalse(process.is_alive(), "child did not exit")
        self.assertEqual(process.exitcode, 0)

    @staticmethod
    def _join_or_kill(process: SpawnProcess) -> None:
        """Bound a child lifetime and reap it even when a test assertion fails."""
        process.join(10)
        if process.is_alive():
            process.terminate()
            process.join(2)
        if process.is_alive():
            process.kill()
            process.join(2)

    def _exclusive_lock_available(self) -> bool:
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

    def test_shared_invocations_run_together(self) -> None:
        """Shared cache-lock modes do not block one another."""
        first, _first_acquired, first_release = self._start_holder(exclusive=False, command="run")
        second: SpawnProcess | None = None
        second_release: Event | None = None
        try:
            second, _second_acquired, second_release = self._start_holder(
                exclusive=False,
                command="verify",
            )
        finally:
            if second is not None and second_release is not None:
                self._stop(second, second_release)
            self._stop(first, first_release)

    def test_blocked_invocation_reports_owner_then_continues(self) -> None:
        """A shared command waits for an exclusive build and resumes after it exits."""
        owner, _owner_acquired, owner_release = self._start_holder(
            exclusive=True,
            profile="usb-host-lab",
        )
        blocked = self.context.Event()
        acquired = self.context.Event()
        release = self.context.Event()
        output = self.context.Queue()
        waiter = self.context.Process(
            target=_wait_for_lock,
            args=(str(self.cache_root), blocked, acquired, release, output),
        )
        waiter.start()
        try:
            self.assertTrue(blocked.wait(5), "waiter did not observe lock contention")
            self.assertFalse(acquired.is_set(), "waiter bypassed the exclusive lock")
            owner_release.set()
            self._join_or_kill(owner)
            self.assertFalse(owner.is_alive(), "owner did not exit")
            self.assertEqual(owner.exitcode, 0)
            self.assertTrue(acquired.wait(5), "waiter did not continue after release")
            release.set()
            self._join_or_kill(waiter)
            self.assertFalse(waiter.is_alive(), "waiter did not exit")
            self.assertEqual(waiter.exitcode, 0)
            rendered = output.get(timeout=5)
            self.assertIn("command=build", rendered)
            self.assertIn("target=demo-target", rendered)
            self.assertIn("profile=usb-host-lab", rendered)
            self.assertIn(f"pid={owner.pid}", rendered)
            self.assertIn("started=", rendered)
            self.assertIn("cache released; continuing", rendered)
        finally:
            owner_release.set()
            release.set()
            self._join_or_kill(owner)
            self._join_or_kill(waiter)
            output.close()
            output.join_thread()

    def test_context_releases_after_return_exception_and_interrupt(self) -> None:
        """Normal return, errors, and Ctrl-C all leave the flock available."""
        cases = (None, RuntimeError, KeyboardInterrupt)
        for exception_type in cases:
            with self.subTest(exception_type=exception_type):
                if exception_type is None:
                    with cache_lock(
                        self.cache_root,
                        exclusive=True,
                        command="build",
                        target="demo-target",
                    ):
                        pass
                else:
                    with (
                        self.assertRaises(exception_type),
                        cache_lock(
                            self.cache_root,
                            exclusive=True,
                            command="build",
                            target="demo-target",
                        ),
                    ):
                        raise exception_type()
                process, _acquired, child_release = self._start_holder(exclusive=True)
                self._stop(process, child_release)

    def test_shared_lock_survives_exec_until_runner_exits(self) -> None:
        """A runner reached through exec keeps build from taking EX too early."""
        acquired_path = self.cache_root / "exec-acquired"
        release_path = self.cache_root / "exec-release"
        process = self.context.Process(
            target=_exec_with_shared_lock,
            args=(str(self.cache_root), str(acquired_path), str(release_path)),
        )
        process.start()
        try:
            deadline = time.monotonic() + 5
            while not acquired_path.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(acquired_path.exists(), "child did not reach exec")
            self.assertFalse(
                self._exclusive_lock_available(),
                "exec'd process lost its shared cache lock",
            )
        finally:
            release_path.touch()
            self._join_or_kill(process)
        self.assertFalse(process.is_alive(), "exec'd child did not exit")
        self.assertEqual(process.exitcode, 0)
        self.assertTrue(
            self._exclusive_lock_available(),
            "shared cache lock remained after runner exit",
        )


if __name__ == "__main__":
    unittest.main()
