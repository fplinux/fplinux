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
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.common import ROOT
from fplinux_cli.reporting.run import RunReporter

from tests.process import process_state, python_environment, run_process

_PROCESS_TIMEOUT = 8.0
_STAGE_TIMEOUT = 5.0
_PROCESS_FIXTURES = ROOT / "tests" / "fixtures" / "processes"


def _wait_for_path(path: Path, deadline: float, description: str) -> None:
    while not path.exists():
        if time.monotonic() >= deadline:
            raise AssertionError(f"{description} did not become ready")
        time.sleep(0.01)


def _kill_recorded_process_group(path: Path) -> None:
    """Kill a Stage-owned group recorded by its isolated child."""
    if not path.exists():
        return
    process_group = int(path.read_text())
    with contextlib.suppress(ProcessLookupError):
        os.killpg(process_group, signal.SIGKILL)
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if process_state(process_group) in {None, "Z"}:
            return
        time.sleep(0.01)


def _wait_until_not_running(process_id: int, deadline: float) -> None:
    while time.monotonic() < deadline:
        state = process_state(process_id)
        if state is None or state == "Z":
            return
        time.sleep(0.01)
    raise AssertionError(f"process {process_id} remained in state {process_state(process_id)}")


class StageProcessTests:
    """Exercise Stage against real isolated child processes."""

    @pytest.mark.parametrize(
        ("mode", "status"),
        [("io", 1), ("io-stage", 1), ("interrupt", 130)],
        ids=["io", "io-stage", "interrupt"],
    )
    def test_entrypoint_reports_io_errors_and_sigint_without_tracebacks(
        self, mode: str, status: int
    ) -> None:
        """Expected failures retain their status and diagnostics without Python stacks."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = run_process(
                [
                    sys.executable,
                    str(_PROCESS_FIXTURES / "reporter_failure.py"),
                    mode,
                    str(root),
                ],
                name="run entrypoint failure fixture",
                env=python_environment(),
                timeout=_PROCESS_TIMEOUT,
            )
            assert (result.returncode) == (status), result.stderr
            assert (result.stdout) == ("")
            assert ("Traceback") not in (result.stderr)
            if mode.startswith("io"):
                assert ("fplinux:") in (result.stderr)
                assert ("missing-input") in (result.stderr)
            if mode == "io-stage":
                log = (root / "run/01-read-input.log").read_text()
                assert ("missing-input") in (log)
                assert ("Traceback") not in (log)
                receipt = json.loads((root / "run/run.json").read_text())
                assert (receipt["status"]) == ("failed")

    def test_quiet_stage_keeps_high_volume_output_in_log(self) -> None:
        """Drain both large child streams without printing their contents."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            stdout = io.StringIO()
            stderr = io.StringIO()
            with (
                contextlib.redirect_stdout(stdout),
                contextlib.redirect_stderr(stderr),
                reporter.stage("volume") as stage,
            ):
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
            assert (b"o" * 1000) in (data)
            assert (b"e" * 1000) in (data)
            assert (stdout.getvalue()) == ("")
            assert ("eee") not in (stderr.getvalue())
            assert ("check: volume OK") in (stderr.getvalue())

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
            assert ("stdout marker") in (stdout.getvalue())
            assert ("stderr marker") not in (stdout.getvalue())
            assert ("stderr marker") in (stderr.getvalue())
            assert ("build target: verbose OK") in (stderr.getvalue())

    def test_passthrough_survives_broken_pipe_from_terminal_stream(self) -> None:
        """A terminal stream raising BrokenPipeError fails neither the stage nor its log.

        BrokenPipeStream is a stub without a file descriptor; it does not exercise a
        real closed pipe or the redirection of its descriptor to /dev/null.
        """

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
            assert (b"stdout marker") in ((root / "01-passthrough.log").read_bytes())
            assert ("build target: passthrough OK") in (terminal.getvalue())

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
            assert (result.returncode) == (7)
            assert (result.stdout) == (b"captured stdout\n")
            assert (result.stderr) == (b"captured stderr\n")
            assert ("captured stdout") in (stdout.getvalue())
            assert ("captured stderr") in (stderr.getvalue())
            log = (root / "01-capture.log").read_bytes()
            assert (b"captured stdout") in (log)
            assert (b"captured stderr") in (log)

    def test_failed_stage_preserves_status_and_prints_log_location(self) -> None:
        """Keep a child exit status and show its diagnostic log location."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            with (
                contextlib.redirect_stderr(terminal),
                pytest.raises(SystemExit) as raised,
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
            assert (raised.value.code) == (7)
            assert ("diagnostic") in ((root / "01-failure.log").read_text())
            assert ("FAILED (exit 7)") in (terminal.getvalue())
            assert ("full log: .cache/logs/test/01-failure.log") in (terminal.getvalue())

    def test_failure_tail_is_limited_and_sanitized(self) -> None:
        """Bound displayed tails while retaining the original log bytes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            with (
                contextlib.redirect_stderr(terminal),
                pytest.raises(SystemExit),
                reporter.stage("tail") as stage,
            ):
                stage.run(
                    [sys.executable, str(_PROCESS_FIXTURES / "failure_tail.py")],
                    timeout=_STAGE_TIMEOUT,
                )
            output = terminal.getvalue()
            assert ("line-000") not in (output)
            assert ("line-099") in (output)
            assert ("?red? invalid=?") in (output)
            assert ("utf8=проверка") in (output)
            assert ("[31m") not in (output)
            assert ("\x1b") not in (output)
            log = (root / "01-tail.log").read_bytes()
            assert (b"\x1b[31mred\x1b[0m invalid=\xff") in (log)

    def test_timeout_kills_child_group_and_records_named_failure(self) -> None:
        """Bound a hung stage, kill its group, and retain a useful diagnostic."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            root = directory / "run"
            child_pid_path = directory / "child.pid"
            descendant_pid_path = directory / "descendant.pid"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            with (
                contextlib.redirect_stderr(terminal),
                pytest.raises(subprocess.TimeoutExpired),
                reporter.stage("bounded child") as stage,
            ):
                stage.run(
                    [sys.executable, str(_PROCESS_FIXTURES / "timeout_tree.py"), str(directory)],
                    timeout=0.5,
                )

            child_pid = int(child_pid_path.read_text())
            descendant_pid = int(descendant_pid_path.read_text())
            deadline = time.monotonic() + 2
            _wait_until_not_running(child_pid, deadline)
            _wait_until_not_running(descendant_pid, deadline)
            log = (root / "01-bounded-child.log").read_text()
            assert ("fplinux: command timed out after 0.5s") in (log)
            assert ("FAILED") in (terminal.getvalue())
            metadata = json.loads(reporter.metadata_path.read_text())
            assert (metadata["status"]) == ("failed")
            assert (metadata["stages"][0]["status"]) == ("failed")
            assert (metadata["stages"][0]["exit"]) is None


class StageSignalProcessTests:
    """Exercise signal forwarding through an isolated Stage wrapper process."""

    def test_repeated_termination_kills_an_unresponsive_child_group(self) -> None:
        """Escalate only after the Stage-owned child group ignores its first SIGTERM."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            child_ready = directory / "child.ready"
            child_group = directory / "child.pgid"
            child_pid_path = directory / "child.pid"
            child_term = directory / "child.term"
            grandchild_pid_path = directory / "grandchild.pid"
            grandchild_term = directory / "grandchild.term"

            def escalate_ready_wrapper(wrapper: subprocess.Popen[str], deadline: float) -> None:
                _wait_for_path(child_ready, deadline, "stage child")
                os.kill(wrapper.pid, signal.SIGTERM)
                _wait_for_path(child_term, deadline, "child SIGTERM receipt")
                _wait_for_path(grandchild_term, deadline, "grandchild SIGTERM receipt")
                os.kill(wrapper.pid, signal.SIGTERM)

            try:
                result = run_process(
                    [
                        sys.executable,
                        str(_PROCESS_FIXTURES / "stage_wrapper.py"),
                        str(directory),
                        "check",
                        "signal escalation",
                        sys.executable,
                        str(_PROCESS_FIXTURES / "ignoring_tree.py"),
                        str(directory),
                    ],
                    name="stage repeated SIGTERM escalation",
                    timeout=_PROCESS_TIMEOUT,
                    cwd=ROOT,
                    env=python_environment(),
                    while_running=escalate_ready_wrapper,
                )
                assert (result.returncode) == (128 + signal.SIGTERM), result.stderr
                child_pid = int(child_pid_path.read_text())
                grandchild_pid = int(grandchild_pid_path.read_text())
                deadline = time.monotonic() + 2
                _wait_until_not_running(child_pid, deadline)
                _wait_until_not_running(grandchild_pid, deadline)
            finally:
                # Cleanup runs only after the assertions so that it cannot satisfy them.
                _kill_recorded_process_group(child_group)
            assert ("FAILED (exit 143)") in (result.stderr)

    def test_signal_is_forwarded_to_every_process_in_the_child_group(self) -> None:
        """Forward SIGTERM from a wrapper to its child and descendant."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            child_ready = directory / "child.ready"
            child_group = directory / "child.pgid"
            child_signal = directory / "child.signal"
            grandchild_signal = directory / "grandchild.signal"

            def terminate_ready_wrapper(wrapper: subprocess.Popen[str], deadline: float) -> None:
                _wait_for_path(child_ready, deadline, "stage child")
                os.kill(wrapper.pid, signal.SIGTERM)

            try:
                result = run_process(
                    [
                        sys.executable,
                        str(_PROCESS_FIXTURES / "stage_wrapper.py"),
                        str(directory),
                        "check",
                        "signal",
                        sys.executable,
                        str(_PROCESS_FIXTURES / "terminating_tree.py"),
                        str(directory),
                    ],
                    name="stage SIGTERM forwarding",
                    timeout=_PROCESS_TIMEOUT,
                    cwd=ROOT,
                    env=python_environment(),
                    while_running=terminate_ready_wrapper,
                )
            finally:
                _kill_recorded_process_group(child_group)
            assert (result.returncode) == (128 + signal.SIGTERM), result.stderr
            assert child_signal.exists(), "direct child did not receive SIGTERM"
            assert grandchild_signal.exists(), "grandchild did not receive SIGTERM"
            assert ("FAILED (exit 143)") in (result.stderr)
            metadata = json.loads((directory / "run" / "run.json").read_text())
            assert (metadata["status"]) == ("interrupted")
            assert (metadata["stages"][0]["status"]) == ("interrupted")
            assert (metadata["stages"][0]["exit"]) == (128 + signal.SIGTERM)

    def test_hangup_is_forwarded_to_a_ready_child(self) -> None:
        """Forward SIGHUP only after the isolated child installs its handler."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            child_ready = directory / "child.ready"
            child_group = directory / "child.pgid"
            child_signal = directory / "child.signal"

            def hangup_ready_wrapper(wrapper: subprocess.Popen[str], deadline: float) -> None:
                _wait_for_path(child_ready, deadline, "stage child")
                os.kill(wrapper.pid, signal.SIGHUP)

            try:
                result = run_process(
                    [
                        sys.executable,
                        str(_PROCESS_FIXTURES / "stage_wrapper.py"),
                        str(directory),
                        "check",
                        "hangup",
                        sys.executable,
                        str(_PROCESS_FIXTURES / "hangup_child.py"),
                        str(directory),
                    ],
                    name="stage SIGHUP forwarding",
                    timeout=_PROCESS_TIMEOUT,
                    cwd=ROOT,
                    env=python_environment(),
                    while_running=hangup_ready_wrapper,
                )
            finally:
                _kill_recorded_process_group(child_group)
            assert (result.returncode) == (128 + signal.SIGHUP), result.stderr
            assert (child_signal.read_text()) == ("SIGHUP")

    def test_job_control_stops_and_resumes_the_child_group(self) -> None:
        """SIGTSTP stops the Stage child group and a forwarded SIGCONT resumes it.

        Only the child group's state is observed; this does not show that the
        isolated wrapper suspends itself and returns control to a shell.
        """
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            child_pid_path = directory / "child.pid"
            child_group = directory / "child.pgid"

            def suspend_and_resume(wrapper: subprocess.Popen[str], deadline: float) -> None:
                _wait_for_path(child_pid_path, deadline, "stage child")
                child_pid = int(child_pid_path.read_text())
                os.kill(wrapper.pid, signal.SIGTSTP)
                try:
                    state = ""
                    while time.monotonic() < deadline:
                        state = process_state(child_pid) or ""
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
                    [
                        sys.executable,
                        str(_PROCESS_FIXTURES / "stage_wrapper.py"),
                        str(directory),
                        "test",
                        "job-control",
                        sys.executable,
                        str(_PROCESS_FIXTURES / "job_control_child.py"),
                        str(directory),
                    ],
                    name="stage job-control forwarding",
                    timeout=_PROCESS_TIMEOUT,
                    cwd=ROOT,
                    env=python_environment(),
                    while_running=suspend_and_resume,
                )
            finally:
                _kill_recorded_process_group(child_group)
            assert (result.returncode) == (0), result.stderr


class TestProcessHelperTests:
    """Verify that the shared host-test process boundary is itself bounded."""

    def test_timeout_terminates_and_reaps_the_isolated_process(self) -> None:
        """Name a timeout failure and leave no running wrapper behind."""
        with tempfile.TemporaryDirectory() as temporary:
            pid_path = Path(temporary) / "process.pid"
            ready_path = Path(temporary) / "process.ready"

            def wait_until_ready(_process: subprocess.Popen[str], deadline: float) -> None:
                _wait_for_path(ready_path, deadline, "helper child")

            with pytest.raises(AssertionError, match="named helper process timed out after 1s"):
                run_process(
                    [sys.executable, str(_PROCESS_FIXTURES / "ignoring_process.py"), temporary],
                    name="named helper process",
                    timeout=1,
                    while_running=wait_until_ready,
                )
            process_id = int(pid_path.read_text())
            with pytest.raises(ProcessLookupError):
                os.kill(process_id, 0)
