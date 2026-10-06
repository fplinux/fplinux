# SPDX-License-Identifier: GPL-2.0-only
"""Host-process tests for kernel-check status propagation."""

from __future__ import annotations

import contextlib
import io
import os
import selectors
import signal
import sys
import tempfile
import time
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.common import ROOT
from fplinux_cli.quality.kernel import analysis as kernel_analysis

from tests.process import process_state, python_environment, run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Callable, Iterator


_PROCESS_FIXTURES = ROOT / "tests" / "fixtures" / "processes"


class KernelCheckSubprocessStatusTests:
    """Preserve shell-visible statuses from isolated helper processes."""

    def test_checkpatch_signal_uses_shell_status(self) -> None:
        """Convert a helper SIGTERM return code before raising."""
        terminal = io.StringIO()
        command = [
            sys.executable,
            "-c",
            "import os, signal; os.kill(os.getpid(), signal.SIGTERM)",
        ]
        with contextlib.redirect_stdout(terminal), pytest.raises(SystemExit) as raised:
            kernel_analysis.run_checkpatch(command)
        assert (raised.value.code) == (128 + signal.SIGTERM)
        assert ("checkpatch") in (terminal.getvalue())

    def test_dtbs_signal_uses_shell_status(self) -> None:
        """Convert a helper SIGKILL return code before propagating it."""
        terminal = io.StringIO()
        command = [
            sys.executable,
            "-c",
            "import os, signal; os.kill(os.getpid(), signal.SIGKILL)",
        ]
        with contextlib.redirect_stdout(terminal), pytest.raises(SystemExit) as raised:
            kernel_analysis.run_dtbs_check(command, "test-target")
        assert (raised.value.code) == (128 + signal.SIGKILL)
        assert ("dtbs_check") in (terminal.getvalue())
        assert ("test-target") in (terminal.getvalue())


class KernelContextSchedulerTests:
    """Exercise the real child-process coordinator at its command boundary."""

    @pytest.fixture(autouse=True)
    def _prepare_case(self) -> Iterator[None]:
        """Create explicit FIFO barriers for two controlled worker processes."""
        with ExitStack() as cleanup:
            self.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(self.temporary)
            self.root = Path(self.temporary.name)
            self.ready_selector = selectors.DefaultSelector()
            cleanup.callback(self.ready_selector.close)
            self.ready_descriptors: dict[str, int] = {}
            self.ready_keepalive_descriptors: dict[str, int] = {}
            self.control_descriptors: dict[str, int] = {}
            for target in ("first", "second"):
                ready = self.root / f"{target}.ready"
                control = self.root / f"{target}.control"
                os.mkfifo(ready)
                os.mkfifo(control)
                ready_descriptor = os.open(ready, os.O_RDONLY | os.O_NONBLOCK)
                cleanup.callback(os.close, ready_descriptor)
                ready_keepalive = os.open(ready, os.O_WRONLY | os.O_NONBLOCK)
                cleanup.callback(os.close, ready_keepalive)
                control_descriptor = os.open(control, os.O_RDWR | os.O_NONBLOCK)
                cleanup.callback(os.close, control_descriptor)
                self.ready_descriptors[target] = ready_descriptor
                self.ready_keepalive_descriptors[target] = ready_keepalive
                self.control_descriptors[target] = control_descriptor
                self.ready_selector.register(ready_descriptor, selectors.EVENT_READ, target)
            yield

    def _run_scheduler(
        self,
        while_running: Callable[[subprocess.Popen[str], float], None],
        *,
        stage_first: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        """Run the coordinator in a bounded process group owned by the test helper."""
        return run_process(
            [
                sys.executable,
                str(_PROCESS_FIXTURES / "kernel_scheduler.py"),
                str(self.root),
                "stage" if stage_first else "simple",
            ],
            name="kernel context scheduler",
            timeout=10,
            cwd=ROOT,
            env=python_environment(),
            while_running=while_running,
        )

    def _accept_worker(self) -> tuple[str, int, str, str]:
        """Accept one explicit readiness message from a started worker."""
        ready = self.ready_selector.select(timeout=10)
        assert ready, "kernel context worker did not reach its readiness barrier"
        key, _mask = ready[0]
        self.ready_selector.unregister(key.fd)
        payload = os.read(key.fd, 4096).decode().strip().split("|")
        assert (len(payload)) == (4)
        target, pid, home, temporary = payload
        assert (target) == (key.data)
        return target, int(pid), home, temporary

    def _release_worker(self, target: str, action: bytes) -> None:
        """Release one worker from its explicit control barrier."""
        assert (len(action)) == (1)
        os.write(self.control_descriptors[target], action)

    def _assert_process_reaped(self, process_id: int) -> None:
        """Require a direct worker owned by the coordinator to disappear."""
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if process_state(process_id) is None:
                return
            time.sleep(0.01)
        pytest.fail(f"process {process_id} was not reaped; state={process_state(process_id)}")

    def _assert_process_stopped(self, process_id: int) -> None:
        """Require an adopted descendant to be absent or terminal."""
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            state = process_state(process_id)
            if state is None:
                return
            if state == "Z":
                return
            time.sleep(0.01)
        pytest.fail(f"process {process_id} survived scheduler cleanup in state {state}")

    def test_two_workers_overlap_with_isolated_scratch(self) -> None:
        """Two slots reach the barrier together and use distinct HOME/TMPDIR roots."""

        def release_workers(_process: subprocess.Popen[str], _deadline: float) -> None:
            first = self._accept_worker()
            second = self._accept_worker()
            assert ({first[0], second[0]}) == ({"first", "second"})
            assert (first[2]) != (second[2])
            assert (first[3]) != (second[3])
            assert Path(first[2]).is_dir()
            assert Path(first[3]).is_dir()
            assert Path(second[2]).is_dir()
            assert Path(second[3]).is_dir()
            self._release_worker("first", b"S")
            self._release_worker("second", b"S")

        result = self._run_scheduler(release_workers)
        assert (result.returncode) == (0), result.stderr

    def test_failure_names_context_and_reaps_blocked_sibling(self) -> None:
        """A failing context cancels and reaps a sibling blocked at the barrier."""
        pids: dict[str, int] = {}
        result = self._run_scheduler(self._fail_second_worker(pids))
        assert (result.returncode) != (0)
        assert ("fplinux: kernel check failed: context second exited 23") in (result.stderr)
        self._assert_process_reaped(pids["first"])

    def _fail_second_worker(
        self, pids: dict[str, int]
    ) -> Callable[[subprocess.Popen[str], float], None]:
        """Record both ready workers and release the second with a failure."""

        def fail_second(_process: subprocess.Popen[str], _deadline: float) -> None:
            for _index in range(2):
                target, pid, _home, _temporary = self._accept_worker()
                pids[target] = pid
            self._release_worker("second", b"F")

        return fail_second

    def test_failure_kills_worker_stage_tool_and_grandchild(self) -> None:
        """Repeated cancellation reaches the active Stage-owned command group."""
        pids: dict[str, int] = {}
        tool_pid_path = self.root / "tool.pid"
        try:
            result = self._run_scheduler(self._fail_second_worker(pids), stage_first=True)
            assert (result.returncode) != (0)
            assert ("fplinux: kernel check failed: context second exited 23") in (result.stderr)
            assert (self.root / "tool.term").exists()
            assert (self.root / "grandchild.term").exists()
            self._assert_process_reaped(pids["first"])
            self._assert_process_reaped(int(tool_pid_path.read_text()))
            self._assert_process_stopped(int((self.root / "grandchild.pid").read_text()))
        finally:
            # Cleanup runs only after the assertions so that it cannot satisfy them.
            if tool_pid_path.exists():
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(int(tool_pid_path.read_text()), signal.SIGKILL)
