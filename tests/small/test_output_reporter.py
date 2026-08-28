# SPDX-License-Identifier: GPL-2.0-only
"""Small tests for compact stage reporting and persistent metadata."""

from __future__ import annotations

import contextlib
import io
import json
import os
import signal
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
            reader_ready = threading.Event()
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
                        reader_ready.set()
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
                self.assertTrue(reader_ready.wait(2), "metadata reader did not become ready")
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
