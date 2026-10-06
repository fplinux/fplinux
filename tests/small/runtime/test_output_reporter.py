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
from datetime import UTC, datetime
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.common import ROOT, display_text
from fplinux_cli.reporting.process import exit_status
from fplinux_cli.reporting.run import RunReporter, run_entrypoint


class RunReporterTests:
    """Exercise the reporter without invoking the build container."""

    def test_create_avoids_same_process_log_collisions(self) -> None:
        """Suffix run directories when a process creates two in one second."""
        with tempfile.TemporaryDirectory() as temporary:
            fixed = datetime(2026, 8, 9, 15, 30, tzinfo=UTC)
            with (
                mock.patch("fplinux_cli.reporting.run.ROOT", Path(temporary)),
                mock.patch("fplinux_cli.reporting.run.datetime") as clock,
            ):
                clock.now.return_value = fixed
                first = RunReporter.create("check", target=None, verbose=False)
                second = RunReporter.create("check", target=None, verbose=False)
            assert (first.root) != (second.root)
            assert (second.root.name) == (f"{first.root.name}-1")

    def test_run_metadata_tracks_stages_and_success(self) -> None:
        """Publish invocation-derived state at creation, stage boundaries, and finish."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            initial = json.loads(reporter.metadata_path.read_text())
            assert (initial["label"]) == ("check")
            assert (initial["pid"]) == (os.getpid())
            assert (initial["status"]) == ("running")
            assert (initial["finished_at"]) is None
            assert (initial["stages"]) == ([])
            assert (initial["display_root"]) == (".cache/logs/test")
            assert (initial["parent"]) is None
            with reporter.stage("prepare"):
                entered = json.loads(reporter.metadata_path.read_text())
                assert (entered["status"]) == ("running")
                assert (entered["stages"]) == (
                    [
                        {
                            "exit": None,
                            "log": "01-prepare.log",
                            "name": "prepare",
                            "status": "running",
                        }
                    ]
                )

            before_finish = json.loads(reporter.metadata_path.read_text())
            assert (before_finish["status"]) == ("running")
            assert (before_finish["finished_at"]) is None
            assert (before_finish["stages"][0]["status"]) == ("success")
            reporter.finish()
            completed = json.loads(reporter.metadata_path.read_text())
            assert (completed["status"]) == ("success")
            assert (completed["finished_at"]) is not None
            assert (completed["stages"][0]["status"]) == ("success")
            assert (completed["stages"][0]["exit"]) == (0)

    def test_run_metadata_marks_stage_failure_without_later_false_success(self) -> None:
        """Keep a caught failed stage failed even if a caller later invokes finish."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "run"
            reporter = RunReporter("check", root, ".cache/logs/test", verbose=False)
            with pytest.raises(SystemExit), reporter.stage("failure"):
                raise SystemExit(7)
            reporter.finish()

            metadata = json.loads(reporter.metadata_path.read_text())
            assert (metadata["status"]) == ("failed")
            assert (metadata["finished_at"]) is not None
            assert (metadata["stages"]) == (
                [
                    {
                        "exit": 7,
                        "log": "01-failure.log",
                        "name": "failure",
                        "status": "failed",
                    }
                ]
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
                assert reader_ready.wait(2), "metadata reader did not become ready"
                with contextlib.redirect_stderr(io.StringIO()):
                    for sequence in range(100):
                        with reporter.stage(f"update-{sequence}"):
                            pass
            finally:
                stop.set()
                reader.join(5)

            assert not (reader.is_alive()), "metadata reader did not stop"
            assert (failures) == ([])
            assert observations
            assert (
                [
                    path.name
                    for path in root.iterdir()
                    if path.name != "run.json" and path.suffix != ".log"
                ]
            ) == ([])

    def test_nested_reporter_writes_only_its_own_metadata(self) -> None:
        """Keep container subreports out of the host run's metadata writer domain."""
        with tempfile.TemporaryDirectory() as temporary:
            host_root = Path(temporary) / "host"
            host = RunReporter("check", host_root, ".cache/logs/check/run", verbose=False)
            environment = host.container_environment(str(host_root))
            with mock.patch.dict(os.environ, environment, clear=False):
                nested = RunReporter.from_environment("check", "quality")
            assert (nested) is not None
            if nested is None:
                return
            with nested.stage("source-inventory"):
                pass
            nested.finish()

            host_metadata = json.loads(host.metadata_path.read_text())
            nested_metadata = json.loads(nested.metadata_path.read_text())
            assert (host_metadata["status"]) == ("running")
            assert (host_metadata["stages"]) == ([])
            assert (nested.root) == (host_root / "quality")
            assert (nested_metadata["display_root"]) == (".cache/logs/check/run/quality")
            assert (nested_metadata["parent"]) == (".cache/logs/check/run")
            assert (nested_metadata["status"]) == ("success")

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
            assert (metadata["status"]) == ("success")
            assert (metadata["finished_at"]) is not None
            assert (metadata["stages"][0]["status"]) == ("success")

    def test_exit_status_converts_signal_return_codes(self) -> None:
        """Expose shell-style statuses for normal and signalled children."""
        assert (exit_status(7)) == (7)
        assert (exit_status(-signal.SIGTERM)) == (128 + signal.SIGTERM)

    def test_display_text_redacts_only_checkout_paths(self) -> None:
        """Keep relative workspace filenames intact while hiding the checkout."""
        assert (display_text("scripts/fplinux_cli/workspace.py")) == (
            "scripts/fplinux_cli/workspace.py"
        )
        assert (display_text(ROOT / "scripts/fplinux_cli/workspace.py")) == (
            "<source-root>/scripts/fplinux_cli/workspace.py"
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
                pytest.raises(RuntimeError, match=message),
                reporter.stage("internal"),
            ):
                raise RuntimeError(message)
            log = (root / "01-internal.log").read_text()
            assert ("Traceback (most recent call last):") in (log)
            assert ("RuntimeError: internal failure") in (log)

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

            with contextlib.redirect_stderr(terminal), pytest.raises(SystemExit) as raised:
                run_entrypoint(fail_inside_stage)
            assert (raised.value.code) == (1)
            assert (terminal.getvalue().count("Traceback (most recent call last):")) == (1)

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

            with contextlib.redirect_stderr(terminal), pytest.raises(SystemExit) as raised:
                run_entrypoint(fail_inside_stage)
            assert (raised.value.code) == (1)
            assert (terminal.getvalue().count(message)) == (1)

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
                pytest.raises(LookupError, match=later_message),
            ):
                run_entrypoint(catch_then_fail)

    def test_nested_run_does_not_take_ownership_of_a_later_parent_failure(self) -> None:
        """A completed child stays successful while its caller's later error is recorded."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            def nested_command() -> None:
                parent = RunReporter("prepare", root / "parent", "parent", verbose=False)
                with parent.stage("build"):
                    child = RunReporter("build", root / "child", "child", verbose=False)
                    with child.stage("compile"):
                        pass
                    child.finish()
                message = "parent cannot publish"
                raise SystemExit(message)

            with (
                contextlib.redirect_stderr(io.StringIO()),
                pytest.raises(SystemExit, match="parent cannot publish"),
            ):
                run_entrypoint(nested_command)
            parent = json.loads((root / "parent/run.json").read_text())
            child = json.loads((root / "child/run.json").read_text())
            assert (parent["status"]) == ("failed")
            assert (child["status"]) == ("success")

    @pytest.mark.parametrize(
        "inner_tail", [pytest.param(False, id="hidden-tail"), pytest.param(True, id="shown-tail")]
    )
    def test_nested_expected_failure_prints_its_cause_once_without_outer_tail(
        self, *, inner_tail: bool
    ) -> None:
        """The cause stays visible once whether the inner stage shows its log tail or not."""
        with tempfile.TemporaryDirectory() as temporary:
            terminal = io.StringIO()

            def fail_in_child_stage(*, show_inner_tail: bool = inner_tail) -> None:
                reporter = RunReporter("prepare", Path(temporary) / "run", "test", verbose=False)
                with (
                    reporter.stage("outer", show_tail=False),
                    reporter.stage("inner", show_tail=show_inner_tail),
                ):
                    message = "cannot read the requested source"
                    raise SystemExit(message)

            with contextlib.redirect_stderr(terminal), pytest.raises(SystemExit) as raised:
                run_entrypoint(fail_in_child_stage)
            assert (raised.value.code) == (1)
            assert (terminal.getvalue().count("cannot read the requested source")) == (1)

    @pytest.mark.parametrize(
        "verbose", [pytest.param(True, id="verbose"), pytest.param(False, id="quiet")]
    )
    def test_container_environment_preserves_display_path_and_verbosity(
        self, *, verbose: bool
    ) -> None:
        """A nested reporter writes under the mounted path but names the host-facing one."""
        with tempfile.TemporaryDirectory() as temporary:
            host = RunReporter(
                "build target",
                Path(temporary) / "host",
                ".cache/logs/build/target/run",
                verbose=verbose,
            )
            mounted = Path(temporary) / "mounted"
            environment = host.container_environment(str(mounted))
            with mock.patch.dict(os.environ, environment, clear=True):
                nested = RunReporter.from_environment("build", "container")

            assert (nested) is not None
            assert (nested.root) == (mounted / "container")
            assert (nested.display_root) == (".cache/logs/build/target/run/container")
            assert (nested.verbose) is (verbose)
