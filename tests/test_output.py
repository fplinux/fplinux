# SPDX-License-Identifier: GPL-2.0-only
"""Tests for compact stage output and persistent diagnostic logs."""

from __future__ import annotations

import contextlib
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from datetime import UTC, datetime
from pathlib import Path
from unittest import mock

from fplinux_cli.common import ROOT, display_text
from fplinux_cli.output import RunReporter, exit_status, run_entrypoint


class RunReporterTests(unittest.TestCase):
    """Exercise the reporter without invoking the build container."""

    def test_create_avoids_same_process_log_collisions(self) -> None:
        """Suffix run directories when a process creates two in one second."""
        with tempfile.TemporaryDirectory() as temporary:
            fixed = datetime(2026, 8, 9, 15, 30, tzinfo=UTC)
            with (
                mock.patch("fplinux_cli.output.ROOT", Path(temporary)),
                mock.patch("fplinux_cli.output.datetime") as clock,
            ):
                clock.now.return_value = fixed
                first = RunReporter.create("check", target=None, verbose=False)
                second = RunReporter.create("check", target=None, verbose=False)
            self.assertNotEqual(first.root, second.root)
            self.assertEqual(second.root.name, f"{first.root.name}-1")

    def test_run_metadata_tracks_stages_and_success(self) -> None:
        """Publish invocation-derived state at creation, stage boundaries, and finish."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            initial = json.loads(reporter.metadata_path.read_text())
            self.assertEqual(initial["label"], "check")
            self.assertEqual(initial["pid"], os.getpid())
            self.assertEqual(initial["status"], "running")
            self.assertIsNone(initial["finished_at"])
            self.assertEqual(initial["stages"], [])
            self.assertEqual(initial["display_root"], ".cache/logs/test")
            self.assertIsNone(initial["parent"])
            with reporter.stage("prepare"):
                entered = json.loads(reporter.metadata_path.read_text())
                self.assertEqual(entered["status"], "running")
                self.assertEqual(
                    entered["stages"],
                    [
                        {
                            "exit": None,
                            "log": "01-prepare.log",
                            "name": "prepare",
                            "status": "running",
                        }
                    ],
                )

            before_finish = json.loads(reporter.metadata_path.read_text())
            self.assertEqual(before_finish["status"], "running")
            self.assertIsNone(before_finish["finished_at"])
            self.assertEqual(before_finish["stages"][0]["status"], "success")
            reporter.finish()
            completed = json.loads(reporter.metadata_path.read_text())
            self.assertEqual(completed["status"], "success")
            self.assertIsNotNone(completed["finished_at"])
            self.assertEqual(completed["stages"][0]["status"], "success")
            self.assertEqual(completed["stages"][0]["exit"], 0)

    def test_run_metadata_marks_stage_failure_without_later_false_success(self) -> None:
        """Keep a caught failed stage failed even if a caller later invokes finish."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            with self.assertRaises(SystemExit), reporter.stage("failure"):
                raise SystemExit(7)
            reporter.finish()

            metadata = json.loads(reporter.metadata_path.read_text())
            self.assertEqual(metadata["status"], "failed")
            self.assertIsNotNone(metadata["finished_at"])
            self.assertEqual(
                metadata["stages"],
                [
                    {
                        "exit": 7,
                        "log": "01-failure.log",
                        "name": "failure",
                        "status": "failed",
                    }
                ],
            )

    def test_run_metadata_remains_readable_during_repeated_updates(self) -> None:
        """Concurrent readers never observe a missing or partial metadata document."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            stop = threading.Event()
            failures: list[BaseException] = []
            observations: list[dict[str, object]] = []

            def read_metadata() -> None:
                while not stop.is_set():
                    try:
                        value = json.loads(reporter.metadata_path.read_text())
                        if not isinstance(value, dict):
                            failures.append(TypeError("metadata is not an object"))
                            return
                        observations.append(value)
                    except (
                        OSError,
                        UnicodeDecodeError,
                        json.JSONDecodeError,
                    ) as error:
                        failures.append(error)
                        return
                    time.sleep(0)

            reader = threading.Thread(target=read_metadata)
            reader.start()
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    for sequence in range(100):
                        with reporter.stage(f"update-{sequence}"):
                            pass
            finally:
                stop.set()
                reader.join(5)

            self.assertFalse(reader.is_alive(), "metadata reader did not stop")
            self.assertEqual(failures, [])
            self.assertTrue(observations)
            self.assertEqual(
                [
                    path.name
                    for path in root.iterdir()
                    if path.name != "run.json" and path.suffix != ".log"
                ],
                [],
            )

    def test_nested_reporter_writes_only_its_own_metadata(self) -> None:
        """Keep container subreports out of the host run's metadata writer domain."""
        with tempfile.TemporaryDirectory() as temporary:
            host_root = Path(temporary) / "host"
            host = RunReporter("check", host_root, ".cache/logs/check/run", verbose=False)
            environment = host.container_environment(str(host_root))
            with mock.patch.dict(os.environ, environment, clear=False):
                nested = RunReporter.from_environment("check", "quality")
            self.assertIsNotNone(nested)
            if nested is None:
                return
            with nested.stage("source-inventory"):
                pass
            nested.finish()

            host_metadata = json.loads(host.metadata_path.read_text())
            nested_metadata = json.loads(nested.metadata_path.read_text())
            self.assertEqual(host_metadata["status"], "running")
            self.assertEqual(host_metadata["stages"], [])
            self.assertEqual(nested.root, host_root / "quality")
            self.assertEqual(nested_metadata["display_root"], ".cache/logs/check/run/quality")
            self.assertEqual(nested_metadata["parent"], ".cache/logs/check/run")
            self.assertEqual(nested_metadata["status"], "success")

    def test_entrypoint_marks_an_internal_reporter_successful(self) -> None:
        """Finish an unannounced container reporter only after its entrypoint returns."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"

            def entrypoint() -> None:
                reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
                with reporter.stage("container-work"):
                    pass

            run_entrypoint(entrypoint)
            metadata = json.loads((root / "run.json").read_text())
            self.assertEqual(metadata["status"], "success")
            self.assertIsNotNone(metadata["finished_at"])
            self.assertEqual(metadata["stages"][0]["status"], "success")

    def test_exit_status_converts_signal_return_codes(self) -> None:
        """Expose shell-style statuses for normal and signalled children."""
        self.assertEqual(exit_status(7), 7)
        self.assertEqual(exit_status(-signal.SIGTERM), 128 + signal.SIGTERM)

    def test_display_text_redacts_only_checkout_paths(self) -> None:
        """Keep relative workspace filenames intact while hiding the checkout."""
        self.assertEqual(
            display_text("scripts/fplinux_cli/workspace.py"), "scripts/fplinux_cli/workspace.py"
        )
        self.assertEqual(
            display_text(ROOT / "scripts/fplinux_cli/workspace.py"),
            "<source-root>/scripts/fplinux_cli/workspace.py",
        )

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
                    ]
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
                    ]
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
                    [
                        sys.executable,
                        "-c",
                        "print('stdout marker')",
                    ]
                )
            self.assertIn(b"stdout marker", (root / "01-passthrough.log").read_bytes())
            self.assertIn("build target: passthrough OK", terminal.getvalue())

    def test_capture_retains_separate_streams_and_status(self) -> None:
        """Capture policy input without hiding it from verbose output or logs."""
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
                    ]
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
                    ]
                )
            self.assertEqual(raised.exception.code, 7)
            self.assertIn("diagnostic", (root / "01-failure.log").read_text())
            self.assertIn("FAILED (exit 7)", terminal.getvalue())
            self.assertIn("full log: .cache/logs/test/01-failure.log", terminal.getvalue())

    def test_stage_log_keeps_internal_traceback(self) -> None:
        """Retain a traceback when Python code fails inside a stage."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            message = "internal failure"
            with (
                contextlib.redirect_stderr(terminal),
                self.assertRaisesRegex(RuntimeError, message),
                reporter.stage("internal"),
            ):
                raise RuntimeError(message)
            log = (root / "01-internal.log").read_text()
            self.assertIn("Traceback (most recent call last):", log)
            self.assertIn("RuntimeError: internal failure", log)

    def test_entrypoint_does_not_print_reported_traceback_twice(self) -> None:
        """Convert an already reported internal error to a quiet exit status."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            message = "entrypoint failure"

            def fail_inside_stage() -> None:
                with reporter.stage("entrypoint"):
                    raise RuntimeError(message)

            with contextlib.redirect_stderr(terminal), self.assertRaises(SystemExit) as raised:
                run_entrypoint(fail_inside_stage)
            self.assertEqual(raised.exception.code, 1)
            self.assertEqual(terminal.getvalue().count("Traceback (most recent call last):"), 1)

    def test_entrypoint_does_not_repeat_reported_system_exit(self) -> None:
        """Avoid printing a reported string exit again at interpreter shutdown."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            message = "expected failure"

            def fail_inside_stage() -> None:
                with reporter.stage("expected"):
                    raise SystemExit(message)

            with contextlib.redirect_stderr(terminal), self.assertRaises(SystemExit) as raised:
                run_entrypoint(fail_inside_stage)
            self.assertEqual(raised.exception.code, 1)
            self.assertEqual(terminal.getvalue().count(message), 1)

    def test_entrypoint_does_not_hide_a_later_exception(self) -> None:
        """Suppress only the exact exception already written by a stage."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            reported_message = "reported failure"
            later_message = "later failure"

            def report_failure() -> None:
                raise RuntimeError(reported_message)

            def catch_then_fail() -> None:
                try:
                    with reporter.stage("caught"):
                        report_failure()
                except RuntimeError:
                    pass
                raise LookupError(later_message)

            with (
                contextlib.redirect_stderr(terminal),
                self.assertRaisesRegex(LookupError, later_message),
            ):
                run_entrypoint(catch_then_fail)

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
                stage.run([sys.executable, "-c", program])
            output = terminal.getvalue()
            self.assertNotIn("line-000", output)
            self.assertIn("line-099", output)
            self.assertIn("?red? invalid=?", output)
            self.assertIn("utf8=проверка", output)
            self.assertNotIn("[31m", output)
            self.assertNotIn("\x1b", output)
            log = (root / "01-tail.log").read_bytes()
            self.assertIn(b"\x1b[31mred\x1b[0m invalid=\xff", log)

    def test_signal_is_forwarded_to_every_process_in_the_child_group(self) -> None:
        """Forward termination to a child and its descendant process."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            root = directory / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            child_ready = directory / "child.ready"
            child_signal = directory / "child.signal"
            grandchild_ready = directory / "grandchild.ready"
            grandchild_signal = directory / "grandchild.signal"
            grandchild_pid = directory / "grandchild.pid"
            grandchild_program = f"""
import os
import signal
import sys
import time
from pathlib import Path
signal_path = Path({str(grandchild_signal)!r})
def terminate(*_args):
    signal_path.touch()
    raise SystemExit(0)
signal.signal(signal.SIGTERM, terminate)
Path({str(grandchild_pid)!r}).write_text(str(os.getpid()))
Path({str(grandchild_ready)!r}).touch()
time.sleep(10)
"""
            child_program = f"""
import signal
import subprocess
import sys
import time
from pathlib import Path
grandchild = subprocess.Popen([sys.executable, "-c", {grandchild_program!r}])
def terminate(*_args):
    Path({str(child_signal)!r}).touch()
    grandchild.wait(timeout=2)
    raise SystemExit(0)
signal.signal(signal.SIGTERM, terminate)
deadline = time.monotonic() + 5
while not Path({str(grandchild_ready)!r}).exists():
    if time.monotonic() >= deadline:
        raise RuntimeError("grandchild did not become ready")
    time.sleep(0.01)
Path({str(child_ready)!r}).touch()
time.sleep(10)
"""
            sender_errors: list[str] = []

            def send_when_ready() -> None:
                deadline = time.monotonic() + 5
                while not child_ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                if not child_ready.exists():
                    sender_errors.append("child did not become ready")
                    return
                os.kill(os.getpid(), signal.SIGTERM)

            sender = threading.Thread(target=send_when_ready)
            sender.start()
            try:
                with (
                    contextlib.redirect_stderr(terminal),
                    self.assertRaises(SystemExit) as raised,
                    reporter.stage("signal") as stage,
                ):
                    stage.run(
                        [
                            sys.executable,
                            "-c",
                            child_program,
                        ]
                    )
            finally:
                sender.join(5)
                if grandchild_pid.exists() and not grandchild_signal.exists():
                    with contextlib.suppress(ProcessLookupError):
                        os.kill(int(grandchild_pid.read_text()), signal.SIGKILL)
            self.assertFalse(sender.is_alive(), "signal sender did not stop")
            self.assertEqual(sender_errors, [])
            self.assertEqual(raised.exception.code, 128 + signal.SIGTERM)
            self.assertTrue(child_signal.exists(), "direct child did not receive SIGTERM")
            self.assertTrue(grandchild_signal.exists(), "grandchild did not receive SIGTERM")
            self.assertIn("FAILED (exit 143)", terminal.getvalue())
            metadata = json.loads(reporter.metadata_path.read_text())
            self.assertEqual(metadata["status"], "interrupted")
            self.assertEqual(metadata["stages"][0]["status"], "interrupted")
            self.assertEqual(metadata["stages"][0]["exit"], 128 + signal.SIGTERM)

    def test_hangup_is_forwarded_to_a_ready_child(self) -> None:
        """Forward SIGHUP only after the child has installed its handler."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            root = directory / "run"
            child_ready = directory / "child.ready"
            child_signal = directory / "child.signal"
            child_pid = directory / "child.pid"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            terminal = io.StringIO()
            child_program = f"""
import os
import signal
import sys
import time
from pathlib import Path
ready = Path({str(child_ready)!r})
received = Path({str(child_signal)!r})
Path({str(child_pid)!r}).write_text(str(os.getpid()))
def handle_hangup(*_args):
    received.write_text("SIGHUP")
    sys.exit(0)
signal.signal(signal.SIGHUP, handle_hangup)
ready.write_text("ready")
time.sleep(5)
"""
            sender_errors: list[str] = []

            def send_when_ready() -> None:
                deadline = time.monotonic() + 5
                while not child_ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                if not child_ready.exists():
                    sender_errors.append("child did not become ready")
                    return
                os.kill(os.getpid(), signal.SIGHUP)

            sender = threading.Thread(target=send_when_ready)
            sender.start()
            try:
                with (
                    contextlib.redirect_stderr(terminal),
                    self.assertRaises(SystemExit) as raised,
                    reporter.stage("hangup") as stage,
                ):
                    stage.run(
                        [
                            sys.executable,
                            "-c",
                            child_program,
                        ]
                    )
            finally:
                sender.join(5)
                if child_pid.exists() and not child_signal.exists():
                    with contextlib.suppress(ProcessLookupError):
                        os.kill(int(child_pid.read_text()), signal.SIGKILL)
            self.assertFalse(sender.is_alive(), "signal sender did not stop")
            self.assertEqual(sender_errors, [])
            self.assertEqual(raised.exception.code, 128 + signal.SIGHUP)
            self.assertEqual(child_signal.read_text(), "SIGHUP")

    def test_job_control_stops_the_orphaned_child_group(self) -> None:
        """Force-stop a new-session child and resume it through the wrapper."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            child_pid_path = directory / "child.pid"
            child_program = f"""
import os
import signal
import sys
import time
from pathlib import Path
signal.signal(signal.SIGCONT, lambda *_: sys.exit(0))
Path({str(child_pid_path)!r}).write_text(str(os.getpid()))
time.sleep(10)
"""
            wrapper_program = f"""
import sys
from pathlib import Path
from fplinux_cli.output import RunReporter
child = {child_program!r}
reporter = RunReporter("test", Path({str(directory / "run")!r}), "test", verbose=False)
with reporter.stage("job-control") as stage:
    stage.run([sys.executable, "-c", child])
"""
            wrapper = subprocess.Popen(
                [sys.executable, "-c", wrapper_program],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            child_pid: int | None = None
            try:
                deadline = time.monotonic() + 2
                while not child_pid_path.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(child_pid_path.exists())
                child_pid = int(child_pid_path.read_text())

                os.kill(wrapper.pid, signal.SIGTSTP)
                deadline = time.monotonic() + 2
                state = ""
                while time.monotonic() < deadline:
                    status = Path(f"/proc/{child_pid}/status").read_text()
                    state = next(
                        line.split()[1]
                        for line in status.splitlines()
                        if line.startswith("State:")
                    )
                    if state == "T":
                        break
                    time.sleep(0.01)
                self.assertEqual(state, "T")

                os.kill(wrapper.pid, signal.SIGCONT)
                _stdout, stderr = wrapper.communicate(timeout=2)
                self.assertEqual(wrapper.returncode, 0, stderr)
            finally:
                if wrapper.poll() is None:
                    with contextlib.suppress(ProcessLookupError):
                        os.kill(wrapper.pid, signal.SIGCONT)
                    wrapper.kill()
                    wrapper.wait()
                if child_pid is not None:
                    with contextlib.suppress(ProcessLookupError):
                        os.kill(child_pid, signal.SIGKILL)

    def test_container_environment_preserves_display_path(self) -> None:
        """Separate the mounted log path from its host-facing location."""
        with tempfile.TemporaryDirectory() as temporary:
            reporter = RunReporter(
                "build target",
                Path(temporary) / "run",
                ".cache/logs/build/target/run",
                verbose=True,
            )
            self.assertEqual(
                reporter.container_environment("/cache/logs/build/target/run"),
                {
                    "FPLINUX_LOG_ROOT": "/cache/logs/build/target/run",
                    "FPLINUX_LOG_DISPLAY_ROOT": ".cache/logs/build/target/run",
                    "FPLINUX_VERBOSE": "1",
                },
            )


if __name__ == "__main__":
    unittest.main()
