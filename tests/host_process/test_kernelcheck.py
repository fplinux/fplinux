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
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli import kernelcheck
from fplinux_cli.common import ROOT

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Callable


_CONTEXT_HELPER = """
import os
import sys
from pathlib import Path

root = Path(sys.argv[1])
message = "|".join(
    (sys.argv[2], str(os.getpid()), os.environ["HOME"], os.environ["TMPDIR"])
)
with (root / f"{sys.argv[2]}.ready").open("wb", buffering=0) as ready:
    ready.write((message + "\\n").encode())
with (root / f"{sys.argv[2]}.control").open("rb", buffering=0) as control:
    action = control.read(1)
if action == b"F":
    raise SystemExit(23)
if action != b"S":
    raise SystemExit(24)
"""

_IGNORING_GRANDCHILD = """
import os
import signal
import sys
import time
from pathlib import Path

root = Path(sys.argv[1])
Path(root / "grandchild.pid").write_text(str(os.getpid()))
def ignore_term(*_args):
    Path(root / "grandchild.term").touch()
signal.signal(signal.SIGTERM, ignore_term)
Path(root / "grandchild.ready").touch()
time.sleep(30)
"""

_IGNORING_TOOL = f"""
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

root = Path(sys.argv[1])
target = sys.argv[2]
worker_pid = sys.argv[3]
Path(root / "tool.pid").write_text(str(os.getpid()))
grandchild = subprocess.Popen([sys.executable, "-c", {_IGNORING_GRANDCHILD!r}, str(root)])
def ignore_term(*_args):
    Path(root / "tool.term").touch()
signal.signal(signal.SIGTERM, ignore_term)
deadline = time.monotonic() + 5
while not Path(root / "grandchild.ready").exists():
    if time.monotonic() >= deadline:
        raise RuntimeError("grandchild did not become ready")
    time.sleep(0.01)
message = "|".join((target, worker_pid, os.environ["HOME"], os.environ["TMPDIR"]))
with (root / f"{{target}}.ready").open("wb", buffering=0) as ready:
    ready.write((message + "\\n").encode())
time.sleep(30)
"""

_STAGE_CONTEXT_HELPER = f"""
import os
import sys
from pathlib import Path
from fplinux_cli.output import RunReporter

root = Path(sys.argv[1])
target = sys.argv[2]
reporter = RunReporter("check", root / "stage-run", "test", verbose=False)
with reporter.stage("blocked tool") as stage:
    stage.run(
        [sys.executable, "-c", {_IGNORING_TOOL!r}, str(root), target, str(os.getpid())],
        timeout=20,
    )
"""

_SCHEDULER_HELPER = f"""
import sys
from pathlib import Path
from fplinux_cli import kernelcheck

root = Path(sys.argv[1])
stage_first = sys.argv[2] == "stage"
contexts = (("first", None), ("second", None))
def command(target, _profile):
    if stage_first and target == "first":
        return [sys.executable, "-c", {_STAGE_CONTEXT_HELPER!r}, str(root), target]
    return [sys.executable, "-c", {_CONTEXT_HELPER!r}, str(root), target]
kernelcheck._CONTEXT_TERMINATE_TIMEOUT = 0.25
kernelcheck._CONTEXT_KILL_TIMEOUT = 0.5
kernelcheck._run_context_processes(contexts, 2, command_for=command)
"""


def _python_environment() -> dict[str, str]:
    """Expose the current production package to an isolated scheduler process."""
    environment = os.environ.copy()
    existing = environment.get("PYTHONPATH")
    paths = [str(ROOT / "scripts")]
    if existing:
        paths.append(existing)
    environment["PYTHONPATH"] = os.pathsep.join(paths)
    return environment


class KernelCheckSubprocessStatusTests(unittest.TestCase):
    """Preserve shell-visible statuses from isolated helper processes."""

    def test_checkpatch_signal_uses_shell_status(self) -> None:
        """Convert a helper SIGTERM return code before raising."""
        terminal = io.StringIO()
        command = [
            sys.executable,
            "-c",
            "import os, signal; os.kill(os.getpid(), signal.SIGTERM)",
        ]
        with contextlib.redirect_stdout(terminal), self.assertRaises(SystemExit) as raised:
            kernelcheck.run_checkpatch(command)
        self.assertEqual(raised.exception.code, 128 + signal.SIGTERM)
        self.assertIn(f"checkpatch exited {-signal.SIGTERM}\n", terminal.getvalue())

    def test_dtbs_signal_uses_shell_status(self) -> None:
        """Convert a helper SIGKILL return code before propagating it."""
        terminal = io.StringIO()
        command = [
            sys.executable,
            "-c",
            "import os, signal; os.kill(os.getpid(), signal.SIGKILL)",
        ]
        with contextlib.redirect_stdout(terminal), self.assertRaises(SystemExit) as raised:
            kernelcheck.run_dtbs_check(command, "test-target")
        self.assertEqual(raised.exception.code, 128 + signal.SIGKILL)
        self.assertIn(
            f"dtbs_check exited {-signal.SIGKILL}: test-target\n",
            terminal.getvalue(),
        )


class KernelContextSchedulerTests(unittest.TestCase):
    """Exercise the real child-process coordinator at its command boundary."""

    def setUp(self) -> None:
        """Create explicit FIFO barriers for two controlled worker processes."""
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.ready_selector = selectors.DefaultSelector()
        self.ready_descriptors: dict[str, int] = {}
        self.ready_keepalive_descriptors: dict[str, int] = {}
        self.control_descriptors: dict[str, int] = {}
        for target in ("first", "second"):
            ready = self.root / f"{target}.ready"
            control = self.root / f"{target}.control"
            os.mkfifo(ready)
            os.mkfifo(control)
            ready_descriptor = os.open(ready, os.O_RDONLY | os.O_NONBLOCK)
            ready_keepalive = os.open(ready, os.O_WRONLY | os.O_NONBLOCK)
            control_descriptor = os.open(control, os.O_RDWR | os.O_NONBLOCK)
            self.ready_descriptors[target] = ready_descriptor
            self.ready_keepalive_descriptors[target] = ready_keepalive
            self.control_descriptors[target] = control_descriptor
            self.ready_selector.register(ready_descriptor, selectors.EVENT_READ, target)

    def tearDown(self) -> None:
        """Close every FIFO descriptor and its isolated directory."""
        self.ready_selector.close()
        descriptors = (
            *self.ready_descriptors.values(),
            *self.ready_keepalive_descriptors.values(),
            *self.control_descriptors.values(),
        )
        for descriptor in descriptors:
            os.close(descriptor)
        self.temporary.cleanup()

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
                "-c",
                _SCHEDULER_HELPER,
                str(self.root),
                "stage" if stage_first else "simple",
            ],
            name="kernel context scheduler",
            timeout=10,
            cwd=ROOT,
            env=_python_environment(),
            while_running=while_running,
        )

    def _accept_worker(self) -> tuple[str, int, str, str]:
        """Accept one explicit readiness message from a started worker."""
        ready = self.ready_selector.select(timeout=10)
        self.assertTrue(ready, "kernel context worker did not reach its readiness barrier")
        key, _mask = ready[0]
        self.ready_selector.unregister(key.fd)
        payload = os.read(key.fd, 4096).decode().strip().split("|")
        self.assertEqual(len(payload), 4)
        target, pid, home, temporary = payload
        self.assertEqual(target, key.data)
        return target, int(pid), home, temporary

    def _release_worker(self, target: str, action: bytes) -> None:
        """Release one worker from its explicit control barrier."""
        self.assertEqual(len(action), 1)
        os.write(self.control_descriptors[target], action)

    @staticmethod
    def _process_state(process_id: int) -> str | None:
        """Return one Linux process state without signaling the test runner."""
        try:
            status = Path(f"/proc/{process_id}/status").read_text()
        except FileNotFoundError:
            return None
        return next(line.split()[1] for line in status.splitlines() if line.startswith("State:"))

    def _assert_process_reaped(self, process_id: int) -> None:
        """Require a direct worker owned by the coordinator to disappear."""
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if self._process_state(process_id) is None:
                return
            time.sleep(0.01)
        self.fail(f"process {process_id} was not reaped; state={self._process_state(process_id)}")

    def _assert_process_stopped(self, process_id: int) -> None:
        """Require an adopted descendant to be absent or terminal."""
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            state = self._process_state(process_id)
            if state is None:
                return
            if state == "Z":
                return
            time.sleep(0.01)
        self.fail(f"process {process_id} survived scheduler cleanup in state {state}")

    def test_two_workers_overlap_with_isolated_scratch(self) -> None:
        """Two slots reach the barrier together and use distinct HOME/TMPDIR roots."""

        def release_workers(_process: subprocess.Popen[str], _deadline: float) -> None:
            first = self._accept_worker()
            second = self._accept_worker()
            self.assertEqual({first[0], second[0]}, {"first", "second"})
            self.assertNotEqual(first[2], second[2])
            self.assertNotEqual(first[3], second[3])
            self.assertTrue(Path(first[2]).is_dir())
            self.assertTrue(Path(first[3]).is_dir())
            self.assertTrue(Path(second[2]).is_dir())
            self.assertTrue(Path(second[3]).is_dir())
            self._release_worker("first", b"S")
            self._release_worker("second", b"S")

        result = self._run_scheduler(release_workers)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_failure_names_context_and_reaps_blocked_sibling(self) -> None:
        """A failing context cancels and reaps a sibling blocked at the barrier."""
        pids: dict[str, int] = {}

        def fail_second(_process: subprocess.Popen[str], _deadline: float) -> None:
            for _index in range(2):
                target, pid, _home, _temporary = self._accept_worker()
                pids[target] = pid
            self._release_worker("second", b"F")

        result = self._run_scheduler(fail_second)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sparse failed: context second exited 23", result.stderr)
        self._assert_process_reaped(pids["first"])

    def test_failure_kills_worker_stage_tool_and_grandchild(self) -> None:
        """Repeated cancellation reaches the active Stage-owned command group."""
        pids: dict[str, int] = {}

        def fail_second(_process: subprocess.Popen[str], _deadline: float) -> None:
            for _index in range(2):
                target, pid, _home, _temporary = self._accept_worker()
                pids[target] = pid
            self._release_worker("second", b"F")

        try:
            result = self._run_scheduler(fail_second, stage_first=True)
        finally:
            tool_pid_path = self.root / "tool.pid"
            if tool_pid_path.exists():
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(int(tool_pid_path.read_text()), signal.SIGKILL)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sparse failed: context second exited 23", result.stderr)
        self.assertTrue((self.root / "tool.term").exists())
        self.assertTrue((self.root / "grandchild.term").exists())
        process_ids = (
            pids["first"],
            int((self.root / "tool.pid").read_text()),
            int((self.root / "grandchild.pid").read_text()),
        )
        self._assert_process_reaped(process_ids[0])
        self._assert_process_reaped(process_ids[1])
        self._assert_process_stopped(process_ids[2])


if __name__ == "__main__":
    unittest.main()
