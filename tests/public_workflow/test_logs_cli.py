# SPDX-License-Identifier: GPL-2.0-only
"""Read command journals through the public CLI in an isolated source checkout."""

from __future__ import annotations

import json
import os
import selectors
import signal
import tempfile
import time
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.cachelock import cache_lock

from tests.cli_support import prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Callable


class LogsCliTests(unittest.TestCase):
    """Use controlled reporter-format inputs, real files and the real public parser."""

    def setUp(self) -> None:
        """Provide source imports but no Kern installation, cache or ambient run history."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        prepare_cli_checkout(self.root)
        self.logs = self.root / ".cache/logs"

    def run_logs(
        self,
        *arguments: str,
        while_running: Callable[[subprocess.Popen[str], float], None] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        """Execute the real reader with a deadline and process cleanup."""
        return run_process(
            [str(self.root / "fplinux"), "logs", *arguments],
            name="public log reader",
            cwd=self.root,
            timeout=10,
            while_running=while_running,
        )

    def journal(
        self,
        relative: str,
        *,
        status: str = "success",
        started: str = "2026-01-02T10:00:00+00:00",
        parent: str | None = None,
        stages: tuple[tuple[str, str], ...] = (),
    ) -> Path:
        """Publish independent input using the reporter's atomic replacement contract."""
        directory = self.logs / relative
        directory.mkdir(parents=True, exist_ok=True)
        payload = {
            "label": relative.split("/", maxsplit=1)[0],
            "pid": os.getpid(),
            "started_at": started,
            "finished_at": None if status == "running" else "2026-01-02T10:00:12+00:00",
            "status": status,
            "parent": parent,
            "display_root": str(directory),
            "stages": [
                {"name": name, "log": f"{index:02d}-{name}.log", "status": state, "exit": None}
                for index, (name, state) in enumerate(stages, 1)
            ],
        }
        temporary = directory / "run.next"
        temporary.write_text(json.dumps(payload))
        temporary.replace(directory / "run.json")
        return directory

    def await_output(
        self, process: subprocess.Popen[str], deadline: float, marker: bytes
    ) -> bytes:
        """Wait for observed reader output before publishing the next producer state."""
        if process.stdout is None:
            self.fail("reader stdout was not captured")
        observed = b""
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while marker not in observed:
                self.assertGreater(deadline - time.monotonic(), 0, observed)
                if selector.select(max(0, deadline - time.monotonic())):
                    chunk = os.read(process.stdout.fileno(), 65536)
                    self.assertTrue(chunk, observed)
                    observed += chunk
        return observed

    def test_list_filters_and_latest_use_recorded_start_not_mtime(self) -> None:
        """Select invocation context and ignore child reporters and unrelated files."""
        first = self.journal("build/example/profiles/microsd-uboot/old")
        self.journal("check/new", started="2026-01-02T10:00:05+00:00", status="failed")
        self.journal("build/example/profiles/microsd-uboot/old/containers/kernel", parent="old")
        unrelated = self.logs / "notes"
        unrelated.mkdir()
        (unrelated / "private.log").write_text("not a command log\n")
        (unrelated / "run.json").write_text("{}")
        os.utime(first / "run.json", None)

        result = self.run_logs("list")
        self.assertEqual(result.returncode, 0, result.stderr)
        records = result.stdout.splitlines()[1:]
        self.assertEqual(
            records,
            [
                "check/new check - default failed 2026-01-02T10:00:05+00:00 7.0",
                (
                    "build/example/profiles/microsd-uboot/old build example microsd-uboot "
                    "success 2026-01-02T10:00:00+00:00 12.0"
                ),
            ],
        )
        self.assertNotIn(str(self.root), result.stdout)
        filtered = self.run_logs(
            "list",
            "--command",
            "build",
            "--target",
            "example",
            "--profile",
            "microsd-uboot",
            "--status",
            "success",
        )
        self.assertEqual(filtered.returncode, 0, filtered.stderr)
        self.assertEqual(
            [row.split()[0] for row in filtered.stdout.splitlines()[1:]],
            ["build/example/profiles/microsd-uboot/old"],
        )
        shown = self.run_logs("show")
        self.assertEqual(shown.returncode, 0, shown.stderr)
        self.assertEqual(shown.stdout, "check/new: failed\n")

    def test_show_selects_nested_stage_failures_and_bounded_tail(self) -> None:
        """The actual child-tool error is available even when a parent records only progress."""
        run = self.journal(
            "check/example", status="failed", stages=(("source", "failed"), ("prepare", "success"))
        )
        (run / "01-source.log").write_text("parent progress\n")
        (run / "02-prepare.log").write_text("successful preparation\n")
        child = self.journal(
            "check/example/containers/source/quality",
            parent="check/example",
            status="failed",
            stages=(("python", "failed"),),
        )
        (child / "01-python.log").write_text("old line\nactual error\nlast line\n")
        result = self.run_logs("show", "example", "--stage", "python", "--tail", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("containers/source/quality/python [failed]", result.stdout)
        self.assertIn("actual error\nlast line\n", result.stdout)
        self.assertNotIn("old line", result.stdout)
        self.assertNotIn("parent progress", result.stdout)
        failed = self.run_logs("show", "check/example", "--failed")
        self.assertEqual(failed.returncode, 0, failed.stderr)
        self.assertIn("actual error", failed.stdout)
        self.assertIn("parent progress", failed.stdout)
        self.assertNotIn("successful preparation", failed.stdout)

    def test_default_profile_is_a_filter_not_an_omitted_selection(self) -> None:
        """Default selection excludes named profiles on list and on an explicit run ID."""
        self.journal("build/example/base")
        self.journal("build/example/profiles/microsd-uboot/card")
        result = self.run_logs("list", "--profile", "default")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            [row.split()[0] for row in result.stdout.splitlines()[1:]], ["build/example/base"]
        )
        rejected = self.run_logs(
            "show", "build/example/profiles/microsd-uboot/card", "--profile", "default"
        )
        self.assertEqual(rejected.returncode, 1, rejected.stderr)
        self.assertIn("no matching log run", rejected.stderr)

    def test_reader_neither_waits_for_cache_lock_nor_creates_logs(self) -> None:
        """Observing a build must remain possible while a writer holds the real cache lock."""
        self.journal("build/example/run")
        before = sorted(path.relative_to(self.logs) for path in self.logs.rglob("*"))
        with cache_lock(self.root / ".cache", exclusive=True, command="build", target="example"):
            result = self.run_logs("list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(result.stdout.splitlines()[1:]), 1)
        self.assertEqual(
            sorted(path.relative_to(self.logs) for path in self.logs.rglob("*")), before
        )

    def test_empty_unknown_ambiguous_and_invalid_requests_have_clear_outcomes(self) -> None:
        """Empty listing succeeds, missing/ambiguous selections fail and invalid flags return 2."""
        empty = self.run_logs("list")
        self.assertEqual(empty.returncode, 0, empty.stderr)
        self.assertEqual(empty.stdout, "RUN COMMAND TARGET PROFILE STATUS STARTED DURATION\n")
        self.assertFalse(self.logs.exists())
        self.journal("check/same")
        self.journal("test/same")
        for arguments, status, error in (
            (("show", "missing"), 1, "no matching log run"),
            (("show", "same"), 1, "ambiguous run ID"),
            (("show", "check/same", "--stage", "missing"), 1, "stage not found"),
            (("show", "--tail", "-1"), 2, "non-negative integer"),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_logs(*arguments)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertIn(error, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_follow_delivers_appends_new_stages_and_final_bytes_once(self) -> None:
        """Follow binds one run, drains new child stages and stops on recorded completion."""
        relative = "test/live"
        run = self.journal(relative, status="running", stages=(("first", "running"),))
        (run / "01-first.log").write_text("hidden history\nready-marker\n")
        observed = b""

        def publish_after_ready(process: subprocess.Popen[str], deadline: float) -> None:
            nonlocal observed
            observed = self.await_output(process, deadline, b"ready-marker\n")
            with (run / "01-first.log").open("a") as stream:
                stream.write("appended-marker\n")
            self.journal("check/newer", started="2026-01-03T00:00:00+00:00")
            child = self.journal(
                relative + "/containers/tool", parent=relative, stages=(("next", "success"),)
            )
            (child / "01-next.log").write_text("new-stage-first\nnew-stage-last\n")
            self.journal(relative, stages=(("first", "success"),))

        result = self.run_logs("follow", "--tail", "1", while_running=publish_after_ready)
        self.assertEqual(result.returncode, 0, result.stderr)
        combined = observed.decode() + result.stdout
        for marker in (
            "ready-marker\n",
            "appended-marker\n",
            "new-stage-first\n",
            "new-stage-last\n",
        ):
            self.assertEqual(combined.count(marker), 1, combined)
        self.assertNotIn("hidden history", combined)
        self.assertNotIn("check/newer", combined)
        self.assertIn("run finished: success", combined)

    def test_interrupting_follow_does_not_change_the_observed_run(self) -> None:
        """Ctrl+C stops only the reader with status 130 and leaves the journal untouched."""
        run = self.journal("test/live", status="running", stages=(("first", "running"),))
        (run / "01-first.log").write_text("ready-marker\n")
        before = (run / "run.json").read_bytes()

        def interrupt_after_ready(process: subprocess.Popen[str], deadline: float) -> None:
            self.await_output(process, deadline, b"ready-marker\n")
            process.send_signal(signal.SIGINT)

        result = self.run_logs("follow", while_running=interrupt_after_ready)
        self.assertEqual(result.returncode, 130, result.stderr)
        self.assertEqual((run / "run.json").read_bytes(), before)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
