# SPDX-License-Identifier: GPL-2.0-only
"""Host-process tests for stage output, signals, and bounded cleanup."""

from __future__ import annotations

import contextlib
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli.common import ROOT
from fplinux_cli.output import RunReporter

from tests.process import run_process

_PROCESS_TIMEOUT = 8.0
_STAGE_TIMEOUT = 5.0


def _python_environment() -> dict[str, str]:
    environment = os.environ.copy()
    existing = environment.get("PYTHONPATH")
    paths = [str(ROOT / "scripts")]
    if existing:
        paths.append(existing)
    environment["PYTHONPATH"] = os.pathsep.join(paths)
    return environment


def _wait_for_path(path: Path, deadline: float, description: str) -> None:
    while not path.exists():
        if time.monotonic() >= deadline:
            raise AssertionError(f"{description} did not become ready")
        time.sleep(0.01)


def _process_state(process_id: int) -> str | None:
    try:
        status = Path(f"/proc/{process_id}/status").read_text()
    except FileNotFoundError:
        return None
    return next(line.split()[1] for line in status.splitlines() if line.startswith("State:"))


def _kill_recorded_process_group(path: Path) -> None:
    """Kill a Stage-owned group recorded by its isolated child."""
    if not path.exists():
        return
    process_group = int(path.read_text())
    with contextlib.suppress(ProcessLookupError):
        os.killpg(process_group, signal.SIGKILL)
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if _process_state(process_group) in {None, "Z"}:
            return
        time.sleep(0.01)


def _wait_until_not_running(process_id: int, deadline: float) -> None:
    while time.monotonic() < deadline:
        state = _process_state(process_id)
        if state is None or state == "Z":
            return
        time.sleep(0.01)
    raise AssertionError(f"process {process_id} remained in state {_process_state(process_id)}")


class StageProcessTests(unittest.TestCase):
    """Exercise Stage against real isolated child processes."""

    def test_quiet_stage_keeps_high_volume_output_in_log(self) -> None:
        """Drain both large child streams without printing their contents."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            with contextlib.redirect_stderr(terminal), reporter.stage("volume") as stage:
                stage.run(
                    [
                        sys.executable,
                        "-c",
                        (
                            "import sys; "
                            "sys.stdout.write('o' * 200000); "
                            "sys.stderr.write('e' * 200000)"
                        ),
                    ],
                    timeout=_STAGE_TIMEOUT,
                )
            data = (root / "01-volume.log").read_bytes()
            self.assertIn(b"o" * 1000, data)
            self.assertIn(b"e" * 1000, data)
            self.assertNotIn("ooo", terminal.getvalue())
            self.assertIn("check: volume OK", terminal.getvalue())

    def test_verbose_stage_tees_original_streams(self) -> None:
        """Tee verbose child output back to its original terminal stream."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("build target", root, ".cache/logs/test", verbose=True)
            stdout = io.StringIO()
            stderr = io.StringIO()
            with (
                contextlib.redirect_stdout(stdout),
                contextlib.redirect_stderr(stderr),
                reporter.stage("verbose") as stage,
            ):
                stage.run(
                    [
                        sys.executable,
                        "-c",
                        (
                            "import sys; print('stdout marker'); "
                            "print('stderr marker', file=sys.stderr)"
                        ),
                    ],
                    timeout=_STAGE_TIMEOUT,
                )
            self.assertIn("stdout marker", stdout.getvalue())
            self.assertNotIn("stderr marker", stdout.getvalue())
            self.assertIn("stderr marker", stderr.getvalue())
            self.assertIn("build target: verbose OK", stderr.getvalue())

    def test_passthrough_ignores_closed_terminal_pipe(self) -> None:
        """Keep the child running when a passthrough consumer closes stdout."""

        class BrokenPipeBuffer:
            def write(self, data: bytes) -> int:
                return len(data)

            def flush(self) -> None:
                raise BrokenPipeError

        class BrokenPipeStream:
            buffer = BrokenPipeBuffer()

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("build target", root, ".cache/logs/test", verbose=True)
            terminal = io.StringIO()
            with (
                mock.patch.object(sys, "stdout", BrokenPipeStream()),
                contextlib.redirect_stderr(terminal),
                reporter.stage("passthrough", passthrough=True) as stage,
            ):
                stage.run(
                    [sys.executable, "-c", "print('stdout marker')"],
                    timeout=_STAGE_TIMEOUT,
                )
            self.assertIn(b"stdout marker", (root / "01-passthrough.log").read_bytes())
            self.assertIn("build target: passthrough OK", terminal.getvalue())

    def test_capture_retains_separate_streams_and_status(self) -> None:
        """Capture child streams without hiding verbose output or log bytes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=True)
            stdout = io.StringIO()
            stderr = io.StringIO()
            with (
                contextlib.redirect_stdout(stdout),
                contextlib.redirect_stderr(stderr),
                reporter.stage("capture") as stage,
            ):
                result = stage.capture(
                    [
                        sys.executable,
                        "-c",
                        (
                            "import sys; print('captured stdout'); "
                            "print('captured stderr', file=sys.stderr); sys.exit(7)"
                        ),
                    ],
                    timeout=_STAGE_TIMEOUT,
                )
            self.assertEqual(result.returncode, 7)
            self.assertEqual(result.stdout, b"captured stdout\n")
            self.assertEqual(result.stderr, b"captured stderr\n")
            self.assertIn("captured stdout", stdout.getvalue())
            self.assertIn("captured stderr", stderr.getvalue())
            log = (root / "01-capture.log").read_bytes()
            self.assertIn(b"captured stdout", log)
            self.assertIn(b"captured stderr", log)

    def test_failed_stage_preserves_status_and_prints_bounded_tail(self) -> None:
        """Keep a child exit status and show its diagnostic log location."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            with (
                contextlib.redirect_stderr(terminal),
                self.assertRaises(SystemExit) as raised,
                reporter.stage("failure") as stage,
            ):
                stage.run(
                    [
                        sys.executable,
                        "-c",
                        "import sys; print('diagnostic'); raise SystemExit(7)",
                    ],
                    timeout=_STAGE_TIMEOUT,
                )
            self.assertEqual(raised.exception.code, 7)
            self.assertIn("diagnostic", (root / "01-failure.log").read_text())
            self.assertIn("FAILED (exit 7)", terminal.getvalue())
            self.assertIn("full log: .cache/logs/test/01-failure.log", terminal.getvalue())

    def test_failure_tail_is_limited_and_sanitized(self) -> None:
        """Bound displayed tails while retaining the original log bytes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            program = (
                "import sys; "
                "[sys.stdout.write(f'line-{i:03d}\\n') for i in range(100)]; "
                "sys.stdout.flush(); "
                "sys.stdout.buffer.write(b'\\x1b[31mred\\x1b[0m invalid=\\xff\\n'); "
                "sys.stdout.buffer.write('utf8=проверка\\n'.encode()); "
                "raise SystemExit(1)"
            )
            with (
                contextlib.redirect_stderr(terminal),
                self.assertRaises(SystemExit),
                reporter.stage("tail") as stage,
            ):
                stage.run(
                    [sys.executable, "-c", program],
                    timeout=_STAGE_TIMEOUT,
                )
            output = terminal.getvalue()
            self.assertNotIn("line-000", output)
            self.assertIn("line-099", output)
            self.assertIn("?red? invalid=?", output)
            self.assertIn("utf8=проверка", output)
            self.assertNotIn("[31m", output)
            self.assertNotIn("\x1b", output)
            log = (root / "01-tail.log").read_bytes()
            self.assertIn(b"\x1b[31mred\x1b[0m invalid=\xff", log)

    def test_timeout_kills_child_group_and_records_named_failure(self) -> None:
        """Bound a hung stage, kill its group, and retain a useful diagnostic."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            root = directory / "run"
            child_pid_path = directory / "child.pid"
            descendant_pid_path = directory / "descendant.pid"
            descendant_ready = directory / "descendant.ready"
            descendant_program = f"""
import os
import time
from pathlib import Path
Path({str(descendant_pid_path)!r}).write_text(str(os.getpid()))
Path({str(descendant_ready)!r}).touch()
time.sleep(30)
"""
            child_program = f"""
import os
import subprocess
import sys
import time
from pathlib import Path
Path({str(child_pid_path)!r}).write_text(str(os.getpid()))
subprocess.Popen([sys.executable, "-c", {descendant_program!r}])
while not Path({str(descendant_ready)!r}).exists():
    time.sleep(0.01)
time.sleep(30)
"""
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            with (
                contextlib.redirect_stderr(terminal),
                self.assertRaises(subprocess.TimeoutExpired),
                reporter.stage("bounded child") as stage,
            ):
                stage.run([sys.executable, "-c", child_program], timeout=0.5)

            child_pid = int(child_pid_path.read_text())
            descendant_pid = int(descendant_pid_path.read_text())
            deadline = time.monotonic() + 2
            _wait_until_not_running(child_pid, deadline)
            _wait_until_not_running(descendant_pid, deadline)
            log = (root / "01-bounded-child.log").read_text()
            self.assertIn("fplinux: command timed out after 0.5s", log)
            self.assertIn("FAILED", terminal.getvalue())
            metadata = json.loads(reporter.metadata_path.read_text())
            self.assertEqual(metadata["status"], "failed")
            self.assertEqual(metadata["stages"][0]["status"], "failed")
            self.assertIsNone(metadata["stages"][0]["exit"])


class StageSignalProcessTests(unittest.TestCase):
    """Exercise signal forwarding through an isolated Stage wrapper process."""

    def test_repeated_termination_kills_an_unresponsive_child_group(self) -> None:
        """Escalate only after the Stage-owned child group ignores its first SIGTERM."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            child_ready = directory / "child.ready"
            child_group = directory / "child.pgid"
            child_pid_path = directory / "child.pid"
            child_term = directory / "child.term"
            grandchild_ready = directory / "grandchild.ready"
            grandchild_pid_path = directory / "grandchild.pid"
            grandchild_term = directory / "grandchild.term"
            grandchild_program = f"""
import os
import signal
import time
from pathlib import Path
Path({str(grandchild_pid_path)!r}).write_text(str(os.getpid()))
def ignore_term(*_args):
    Path({str(grandchild_term)!r}).touch()
signal.signal(signal.SIGTERM, ignore_term)
Path({str(grandchild_ready)!r}).touch()
time.sleep(30)
"""
            child_program = f"""
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
Path({str(child_pid_path)!r}).write_text(str(os.getpid()))
grandchild = subprocess.Popen([sys.executable, "-c", {grandchild_program!r}])
def ignore_term(*_args):
    Path({str(child_term)!r}).touch()
signal.signal(signal.SIGTERM, ignore_term)
Path({str(child_group)!r}).write_text(str(os.getpgrp()))
deadline = time.monotonic() + 5
while not Path({str(grandchild_ready)!r}).exists():
    if time.monotonic() >= deadline:
        raise RuntimeError("grandchild did not become ready")
    time.sleep(0.01)
Path({str(child_ready)!r}).touch()
time.sleep(30)
"""
            wrapper_program = f"""
import sys
from pathlib import Path
from fplinux_cli.output import RunReporter
reporter = RunReporter("check", Path({str(directory / "run")!r}), "test", verbose=False)
with reporter.stage("signal escalation") as stage:
    stage.run([sys.executable, "-c", {child_program!r}], timeout=10)
"""

            def escalate_ready_wrapper(wrapper: subprocess.Popen[str], deadline: float) -> None:
                _wait_for_path(child_ready, deadline, "stage child")
                os.kill(wrapper.pid, signal.SIGTERM)
                _wait_for_path(child_term, deadline, "child SIGTERM receipt")
                _wait_for_path(grandchild_term, deadline, "grandchild SIGTERM receipt")
                os.kill(wrapper.pid, signal.SIGTERM)

            try:
                result = run_process(
                    [sys.executable, "-c", wrapper_program],
                    name="stage repeated SIGTERM escalation",
                    timeout=_PROCESS_TIMEOUT,
                    cwd=ROOT,
                    env=_python_environment(),
                    while_running=escalate_ready_wrapper,
                )
            finally:
                _kill_recorded_process_group(child_group)
            self.assertEqual(result.returncode, 128 + signal.SIGTERM, result.stderr)
            child_pid = int(child_pid_path.read_text())
            grandchild_pid = int(grandchild_pid_path.read_text())
            deadline = time.monotonic() + 2
            _wait_until_not_running(child_pid, deadline)
            _wait_until_not_running(grandchild_pid, deadline)
            self.assertIn("FAILED (exit 143)", result.stderr)

    def test_signal_is_forwarded_to_every_process_in_the_child_group(self) -> None:
        """Forward SIGTERM from a wrapper to its child and descendant."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            child_ready = directory / "child.ready"
            child_group = directory / "child.pgid"
            child_signal = directory / "child.signal"
            grandchild_ready = directory / "grandchild.ready"
            grandchild_signal = directory / "grandchild.signal"
            grandchild_program = f"""
import signal
import time
from pathlib import Path
def terminate(*_args):
    Path({str(grandchild_signal)!r}).touch()
    raise SystemExit(0)
signal.signal(signal.SIGTERM, terminate)
Path({str(grandchild_ready)!r}).touch()
time.sleep(30)
"""
            child_program = f"""
import signal
import subprocess
import sys
import time
import os
from pathlib import Path
grandchild = subprocess.Popen([sys.executable, "-c", {grandchild_program!r}])
def terminate(*_args):
    Path({str(child_signal)!r}).touch()
    grandchild.wait(timeout=2)
    raise SystemExit(0)
signal.signal(signal.SIGTERM, terminate)
Path({str(child_group)!r}).write_text(str(os.getpgrp()))
deadline = time.monotonic() + 5
while not Path({str(grandchild_ready)!r}).exists():
    if time.monotonic() >= deadline:
        raise RuntimeError("grandchild did not become ready")
    time.sleep(0.01)
Path({str(child_ready)!r}).touch()
time.sleep(30)
"""
            wrapper_program = f"""
import sys
from pathlib import Path
from fplinux_cli.output import RunReporter
reporter = RunReporter("check", Path({str(directory / "run")!r}), "test", verbose=False)
with reporter.stage("signal") as stage:
    stage.run([sys.executable, "-c", {child_program!r}], timeout=10)
"""

            def terminate_ready_wrapper(wrapper: subprocess.Popen[str], deadline: float) -> None:
                _wait_for_path(child_ready, deadline, "stage child")
                os.kill(wrapper.pid, signal.SIGTERM)

            try:
                result = run_process(
                    [sys.executable, "-c", wrapper_program],
                    name="stage SIGTERM forwarding",
                    timeout=_PROCESS_TIMEOUT,
                    cwd=ROOT,
                    env=_python_environment(),
                    while_running=terminate_ready_wrapper,
                )
            finally:
                _kill_recorded_process_group(child_group)
            self.assertEqual(result.returncode, 128 + signal.SIGTERM, result.stderr)
            self.assertTrue(child_signal.exists(), "direct child did not receive SIGTERM")
            self.assertTrue(grandchild_signal.exists(), "grandchild did not receive SIGTERM")
            self.assertIn("FAILED (exit 143)", result.stderr)
            metadata = json.loads((directory / "run" / "run.json").read_text())
            self.assertEqual(metadata["status"], "interrupted")
            self.assertEqual(metadata["stages"][0]["status"], "interrupted")
            self.assertEqual(metadata["stages"][0]["exit"], 128 + signal.SIGTERM)

    def test_hangup_is_forwarded_to_a_ready_child(self) -> None:
        """Forward SIGHUP only after the isolated child installs its handler."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            child_ready = directory / "child.ready"
            child_group = directory / "child.pgid"
            child_signal = directory / "child.signal"
            child_program = f"""
import signal
import sys
import time
import os
from pathlib import Path
def handle_hangup(*_args):
    Path({str(child_signal)!r}).write_text("SIGHUP")
    sys.exit(0)
signal.signal(signal.SIGHUP, handle_hangup)
Path({str(child_group)!r}).write_text(str(os.getpgrp()))
Path({str(child_ready)!r}).write_text("ready")
time.sleep(30)
"""
            wrapper_program = f"""
import sys
from pathlib import Path
from fplinux_cli.output import RunReporter
reporter = RunReporter("check", Path({str(directory / "run")!r}), "test", verbose=False)
with reporter.stage("hangup") as stage:
    stage.run([sys.executable, "-c", {child_program!r}], timeout=10)
"""

            def hangup_ready_wrapper(wrapper: subprocess.Popen[str], deadline: float) -> None:
                _wait_for_path(child_ready, deadline, "stage child")
                os.kill(wrapper.pid, signal.SIGHUP)

            try:
                result = run_process(
                    [sys.executable, "-c", wrapper_program],
                    name="stage SIGHUP forwarding",
                    timeout=_PROCESS_TIMEOUT,
                    cwd=ROOT,
                    env=_python_environment(),
                    while_running=hangup_ready_wrapper,
                )
            finally:
                _kill_recorded_process_group(child_group)
            self.assertEqual(result.returncode, 128 + signal.SIGHUP, result.stderr)
            self.assertEqual(child_signal.read_text(), "SIGHUP")

    def test_job_control_stops_and_resumes_the_child_group(self) -> None:
        """Suspend an isolated wrapper and resume the child through SIGCONT."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            child_pid_path = directory / "child.pid"
            child_group = directory / "child.pgid"
            child_program = f"""
import os
import signal
import sys
import time
from pathlib import Path
signal.signal(signal.SIGCONT, lambda *_: sys.exit(0))
Path({str(child_group)!r}).write_text(str(os.getpgrp()))
Path({str(child_pid_path)!r}).write_text(str(os.getpid()))
time.sleep(30)
"""
            wrapper_program = f"""
import sys
from pathlib import Path
from fplinux_cli.output import RunReporter
reporter = RunReporter("test", Path({str(directory / "run")!r}), "test", verbose=False)
with reporter.stage("job-control") as stage:
    stage.run([sys.executable, "-c", {child_program!r}], timeout=10)
"""

            def suspend_and_resume(wrapper: subprocess.Popen[str], deadline: float) -> None:
                _wait_for_path(child_pid_path, deadline, "stage child")
                child_pid = int(child_pid_path.read_text())
                os.kill(wrapper.pid, signal.SIGTSTP)
                try:
                    state = ""
                    while time.monotonic() < deadline:
                        state = _process_state(child_pid) or ""
                        if state == "T":
                            break
                        time.sleep(0.01)
                    if state != "T":
                        raise AssertionError(f"stage child did not stop; state={state!r}")
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.kill(wrapper.pid, signal.SIGCONT)

            try:
                result = run_process(
                    [sys.executable, "-c", wrapper_program],
                    name="stage job-control forwarding",
                    timeout=_PROCESS_TIMEOUT,
                    cwd=ROOT,
                    env=_python_environment(),
                    while_running=suspend_and_resume,
                )
            finally:
                _kill_recorded_process_group(child_group)
            self.assertEqual(result.returncode, 0, result.stderr)


class TestProcessHelperTests(unittest.TestCase):
    """Verify that the shared host-test process boundary is itself bounded."""

    def test_timeout_terminates_and_reaps_the_isolated_process(self) -> None:
        """Name a timeout failure and leave no running wrapper behind."""
        with tempfile.TemporaryDirectory() as temporary:
            pid_path = Path(temporary) / "process.pid"
            ready_path = Path(temporary) / "process.ready"
            program = f"""
import os
import signal
import time
from pathlib import Path
signal.signal(signal.SIGTERM, signal.SIG_IGN)
Path({str(pid_path)!r}).write_text(str(os.getpid()))
Path({str(ready_path)!r}).touch()
time.sleep(30)
"""

            def wait_until_ready(_process: subprocess.Popen[str], deadline: float) -> None:
                _wait_for_path(ready_path, deadline, "helper child")

            with self.assertRaisesRegex(
                AssertionError,
                "named helper process timed out after 1s",
            ):
                run_process(
                    [sys.executable, "-c", program],
                    name="named helper process",
                    timeout=1,
                    while_running=wait_until_ready,
                )
            process_id = int(pid_path.read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(process_id, 0)


if __name__ == "__main__":
    unittest.main()
