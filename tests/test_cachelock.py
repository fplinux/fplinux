# SPDX-License-Identifier: GPL-2.0-only
"""Multiprocess coverage for the public cache-lock behaviour."""

from __future__ import annotations

import contextlib
import io
import multiprocessing
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.cachelock import cache_lock

if TYPE_CHECKING:
    from multiprocessing.context import SpawnProcess
    from multiprocessing.queues import Queue
    from multiprocessing.synchronize import Event


_CHILD_LOCK_TIMEOUT = "parent did not release child cache lock"
_CHILD_WAIT_TIMEOUT = "parent did not release waiting child"


@dataclass(frozen=True)
class _LockInvocation:
    """The public cache-lock arguments sent to a spawned test process."""

    exclusive: bool
    command: str
    target: str | None


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
    ):
        acquired.set()
        if not release.wait(10):
            raise RuntimeError(_CHILD_LOCK_TIMEOUT)


def _wait_for_lock(
    cache_root: str,
    acquired: Event,
    release: Event,
    output: Queue[str],
) -> None:
    """Capture the wait notice emitted by an invocation that is genuinely blocked."""
    stderr = io.StringIO()
    with (
        contextlib.redirect_stderr(stderr),
        cache_lock(
            Path(cache_root),
            exclusive=False,
            command="package",
            target="nokia-ta1618",
        ),
    ):
        acquired.set()
        if not release.wait(10):
            raise RuntimeError(_CHILD_WAIT_TIMEOUT)
    output.put(stderr.getvalue())


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
        target: str | None = "nokia-ta1618",
    ) -> tuple[SpawnProcess, Event, Event]:
        """Start a process that has acquired the requested lock."""
        acquired = self.context.Event()
        release = self.context.Event()
        process = self.context.Process(
            target=_hold_lock,
            args=(
                str(self.cache_root),
                _LockInvocation(exclusive, command, target),
                acquired,
                release,
            ),
        )
        process.start()
        self.assertTrue(acquired.wait(5), "child did not acquire its cache lock")
        return process, acquired, release

    def _stop(self, process: SpawnProcess, release: Event) -> None:
        """Release and reap a test child, including from assertion cleanup."""
        release.set()
        process.join(10)
        self.assertFalse(process.is_alive(), "child did not exit")
        self.assertEqual(process.exitcode, 0)

    def test_shared_invocations_run_together(self) -> None:
        """The public shared modes do not block one another."""
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
        owner, _owner_acquired, owner_release = self._start_holder(exclusive=True)
        acquired = self.context.Event()
        release = self.context.Event()
        output = self.context.Queue()
        waiter = self.context.Process(
            target=_wait_for_lock,
            args=(str(self.cache_root), acquired, release, output),
        )
        waiter.start()
        try:
            self.assertFalse(acquired.wait(0.3), "waiter bypassed the exclusive lock")
            owner_release.set()
            owner.join(10)
            self.assertFalse(owner.is_alive(), "owner did not exit")
            self.assertEqual(owner.exitcode, 0)
            self.assertTrue(acquired.wait(5), "waiter did not continue after release")
            release.set()
            waiter.join(10)
            self.assertFalse(waiter.is_alive(), "waiter did not exit")
            self.assertEqual(waiter.exitcode, 0)
            rendered = output.get(timeout=5)
            self.assertIn("command=build", rendered)
            self.assertIn("target=nokia-ta1618", rendered)
            self.assertIn(f"pid={owner.pid}", rendered)
            self.assertIn("started=", rendered)
            self.assertIn("cache released; continuing", rendered)
        finally:
            owner_release.set()
            release.set()
            owner.join(10)
            waiter.join(10)
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
                        target="nokia-ta1618",
                    ):
                        pass
                else:
                    with (
                        self.assertRaises(exception_type),
                        cache_lock(
                            self.cache_root,
                            exclusive=True,
                            command="build",
                            target="nokia-ta1618",
                        ),
                    ):
                        raise exception_type()
                process, _acquired, child_release = self._start_holder(exclusive=True)
                self._stop(process, child_release)


if __name__ == "__main__":
    unittest.main()
