# SPDX-License-Identifier: GPL-2.0-only
"""Read command journals through the public CLI in an isolated source checkout."""

from __future__ import annotations

import json
import os
import selectors
import signal
import time
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.cache.lock import cache_lock
from fplinux_cli.runtime.events import RunLog

from tests.cli_support import prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Callable
    from pathlib import Path


@dataclass(frozen=True)
class LogsCli:
    """Own the checkout and journal directory observed by the public reader."""

    root: Path
    logs: Path

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

    def journal(  # noqa: PLR0913 -- independent journal context, timing and stages stay explicit.
        self,
        relative: str,
        *,
        status: str = "success",
        started: str = "2026-01-02T10:00:00+00:00",
        parent: str | None = None,
        build_type: str | None = None,
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
            "parent": f".cache/logs/{parent}" if parent is not None else None,
            "display_root": directory.relative_to(self.root).as_posix(),
            "stages": [
                {"name": name, "log": f"{index:02d}-{name}.log", "status": state, "exit": None}
                for index, (name, state) in enumerate(stages, 1)
            ],
        }
        if build_type is not None:
            payload["build_type"] = build_type
        temporary = directory / "run.next"
        temporary.write_text(json.dumps(payload))
        temporary.replace(directory / "run.json")
        return directory


@pytest.fixture
def logs_cli(tmp_path: Path) -> LogsCli:
    """Provide source imports but no Kern installation, cache or ambient run history."""
    prepare_cli_checkout(tmp_path)
    return LogsCli(tmp_path, tmp_path / ".cache/logs")


def await_output(process: subprocess.Popen[str], deadline: float, marker: bytes) -> bytes:
    """Wait for observed reader output before publishing the next producer state."""
    if process.stdout is None:
        pytest.fail("reader stdout was not captured")
    observed = b""
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while marker not in observed:
            assert (deadline - time.monotonic()) > (0), observed
            if selector.select(max(0, deadline - time.monotonic())):
                chunk = os.read(process.stdout.fileno(), 65536)
                assert chunk, observed
                observed += chunk
    return observed


class LogsCliTests:
    """Use controlled reporter-format inputs, real files and the real public parser."""

    def test_runtime_writer_remains_readable_without_reporter_tree_metadata(
        self, logs_cli: LogsCli
    ) -> None:
        """The public reader accepts the journal emitted by the live SSH/runtime writer."""
        runtime_root = logs_cli.logs / "run/phone"
        runtime_root.mkdir(parents=True)
        log = RunLog(runtime_root, protect=lambda _path: None)
        log.record("runtime diagnostic")
        log.close("success")
        before = (log.directory / "run.json").read_bytes()

        shown = logs_cli.run_logs("show", "--command", "run", "--tail", "2")

        assert shown.returncode == 0, shown.stderr
        assert "runtime diagnostic" in shown.stdout
        assert "runtime [success]" in shown.stdout
        assert (log.directory / "run.json").read_bytes() == before

    def test_list_filters_and_latest_use_recorded_start_not_mtime(self, logs_cli: LogsCli) -> None:
        """Select invocation context and ignore child reporters and unrelated files."""
        first = logs_cli.journal("build/example/profiles/microsd-uboot/old")
        logs_cli.journal("check/new", started="2026-01-02T10:00:05+00:00", status="failed")
        logs_cli.journal(
            "build/example/profiles/microsd-uboot/old/containers/kernel",
            parent="build/example/profiles/microsd-uboot/old/containers",
        )
        unrelated = logs_cli.logs / "notes"
        unrelated.mkdir()
        (unrelated / "private.log").write_text("not a command log\n")
        (unrelated / "run.json").write_text("{}")
        os.utime(first / "run.json", None)

        result = logs_cli.run_logs("list")
        assert (result.returncode) == (0), result.stderr
        records = result.stdout.splitlines()[1:]
        assert (records) == (
            [
                "check/new check - default unknown failed 2026-01-02T10:00:05+00:00 7.0",
                (
                    "build/example/profiles/microsd-uboot/old build example microsd-uboot "
                    "unknown success 2026-01-02T10:00:00+00:00 12.0"
                ),
            ]
        )
        assert (str(logs_cli.root)) not in (result.stdout)
        filtered = logs_cli.run_logs(
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
        assert (filtered.returncode) == (0), filtered.stderr
        assert ([row.split()[0] for row in filtered.stdout.splitlines()[1:]]) == (
            ["build/example/profiles/microsd-uboot/old"]
        )
        shown = logs_cli.run_logs("show")
        assert (shown.returncode) == (0), shown.stderr
        assert (shown.stdout) == ("check/new: failed; build type: unknown\n")

    def test_show_selects_nested_stage_failures_and_bounded_tail(self, logs_cli: LogsCli) -> None:
        """The actual child-tool error is available even when a parent records only progress."""
        run = logs_cli.journal(
            "check/example", status="failed", stages=(("source", "failed"), ("prepare", "success"))
        )
        (run / "01-source.log").write_text("parent progress\n")
        (run / "02-prepare.log").write_text("successful preparation\n")
        child = logs_cli.journal(
            "check/example/containers/source/quality",
            parent="check/example/containers/source",
            status="failed",
            stages=(("python", "failed"),),
        )
        (child / "01-python.log").write_text("old line\nactual error\nlast line\n")
        result = logs_cli.run_logs("show", "example", "--stage", "python", "--tail", "2")
        assert (result.returncode) == (0), result.stderr
        assert ("containers/source/quality/python [failed]") in (result.stdout)
        assert ("actual error\nlast line\n") in (result.stdout)
        assert ("old line") not in (result.stdout)
        assert ("parent progress") not in (result.stdout)
        failed = logs_cli.run_logs("show", "check/example", "--failed")
        assert (failed.returncode) == (0), failed.stderr
        assert ("actual error") in (failed.stdout)
        assert ("parent progress") in (failed.stdout)
        assert ("successful preparation") not in (failed.stdout)

    def test_default_profile_is_a_filter_not_an_omitted_selection(self, logs_cli: LogsCli) -> None:
        """Default selection excludes named profiles on list and on an explicit run ID."""
        logs_cli.journal("build/example/base")
        logs_cli.journal("build/example/profiles/microsd-uboot/card")
        result = logs_cli.run_logs("list", "--profile", "default")
        assert (result.returncode) == (0), result.stderr
        assert ([row.split()[0] for row in result.stdout.splitlines()[1:]]) == (
            ["build/example/base"]
        )
        rejected = logs_cli.run_logs(
            "show", "build/example/profiles/microsd-uboot/card", "--profile", "default"
        )
        assert (rejected.returncode) == (1), rejected.stderr
        assert ("no matching log run") in (rejected.stderr)

    def test_missing_stage_lists_exact_nested_ids_and_tail_zero_discovers_them(
        self, logs_cli: LogsCli
    ) -> None:
        """A rejected selection names the available IDs without printing stage contents."""
        run = logs_cli.journal("build/example/run", stages=(("container", "success"),))
        (run / "01-container.log").write_text("parent output\n")
        child = logs_cli.journal(
            "build/example/run/container/build",
            parent="build/example/run/container",
            stages=(("rootfs", "success"),),
        )
        (child / "01-rootfs.log").write_text("child output\n")

        missing = logs_cli.run_logs("show", "run", "--stage", "kernel", "--tail", "0")
        discovered = logs_cli.run_logs("show", "run", "--tail", "0")

        assert missing.returncode == 1, missing.stderr
        assert missing.stderr == (
            "fplinux: stage not found: kernel\n"
            "available stage IDs:\n"
            "  container\n"
            "  container/build/rootfs\n"
        )
        assert discovered.returncode == 0, discovered.stderr
        assert "==> container [success] <==" in discovered.stdout
        assert "==> container/build/rootfs [success] <==" in discovered.stdout
        assert "parent output" not in missing.stdout + discovered.stdout
        assert "child output" not in missing.stdout + discovered.stdout

    @pytest.mark.parametrize("action", ["list", "show", "follow"])
    def test_build_type_filter_selects_recorded_context_and_excludes_unknown(
        self, logs_cli: LogsCli, action: str
    ) -> None:
        """Newest debug and unknown runs cannot replace a requested recorded release run."""
        logs_cli.journal("build/example/release", build_type="release")
        logs_cli.journal(
            "build/example/debug", build_type="debug", started="2026-01-02T10:00:01+00:00"
        )
        logs_cli.journal("build/example/unknown", started="2026-01-02T10:00:02+00:00")

        selected = logs_cli.run_logs(action, "--command", "build", "--build-type", "release")

        assert selected.returncode == 0, selected.stderr
        assert "build/example/release" in selected.stdout
        assert "build/example/debug" not in selected.stdout
        assert "build/example/unknown" not in selected.stdout
        if action == "list":
            assert "BUILD_TYPE" in selected.stdout.splitlines()[0]
            assert "release success" in selected.stdout
        else:
            assert "build type: release" in selected.stdout
        all_runs = logs_cli.run_logs("list", "--command", "build")
        assert all_runs.returncode == 0, all_runs.stderr
        assert "build/example/unknown build example default unknown success" in all_runs.stdout

    @pytest.mark.parametrize("parent_status", ["failed", "interrupted", "running"])
    def test_unfinished_child_preserves_recorded_state_and_reports_unsuccessful_parent(
        self, logs_cli: LogsCli, parent_status: str
    ) -> None:
        """Diagnostic selection exposes unfinished work without asserting a child exit."""
        run = logs_cli.journal("build/example/run", status=parent_status)
        child = logs_cli.journal(
            "build/example/run/container/build",
            status="running",
            parent="build/example/run/container",
            stages=(("rootfs", "running"),),
        )
        (child / "01-rootfs.log").write_text("last child output\n")
        before = {path: path.read_bytes() for path in logs_cli.logs.rglob("run.json")}

        shown = logs_cli.run_logs("show", "run")
        failures = logs_cli.run_logs("show", "run", "--failed")

        assert shown.returncode == 0, shown.stderr
        assert failures.returncode == 0, failures.stderr
        if parent_status == "running":
            assert "container/build/rootfs [running]" in shown.stdout
            assert "unfinished" not in shown.stdout
            assert "last child output" not in failures.stdout
            assert "no failed or interrupted stages in this run" in failures.stdout
        else:
            annotation = f"container/build/rootfs [running; unfinished; parent {parent_status}]"
            assert annotation in shown.stdout
            assert annotation in failures.stdout
            assert "last child output" in failures.stdout
        assert {path: path.read_bytes() for path in before} == before
        assert json.loads((run / "run.json").read_text())["status"] == parent_status
        assert json.loads((child / "run.json").read_text())["status"] == "running"

    def test_nested_journal_with_unrelated_recorded_parent_is_not_an_owned_child(
        self, logs_cli: LogsCli
    ) -> None:
        """A directory descendant alone does not establish reporter ownership."""
        logs_cli.journal("build/example/run", status="failed")
        child = logs_cli.journal(
            "build/example/run/container/build",
            status="running",
            parent="build/other/run/container",
            stages=(("rootfs", "running"),),
        )
        (child / "01-rootfs.log").write_text("unrelated child output\n")
        before = (child / "run.json").read_bytes()

        result = logs_cli.run_logs("show", "run", "--failed")

        assert result.returncode == 0, result.stderr
        assert "unrelated child output" not in result.stdout
        assert "unfinished" not in result.stdout
        assert "no failed or interrupted stages in this run" in result.stdout
        assert (child / "run.json").read_bytes() == before

    @pytest.mark.parametrize(
        "stages",
        [(), (("source", "success"),)],
        ids=["cached-run", "successful-stages"],
    )
    def test_show_failed_explains_a_run_without_failed_stages(
        self, logs_cli: LogsCli, stages: tuple[tuple[str, str], ...]
    ) -> None:
        """An empty failure filter explains the result while keeping its successful exit."""
        run = logs_cli.journal("build/example/run", stages=stages)
        if stages:
            (run / "01-source.log").write_text("successful output\n")

        result = logs_cli.run_logs("show", "--command", "build", "--failed")

        assert result.returncode == 0, result.stderr
        assert "build/example/run: success; build type: unknown\n" in result.stdout
        assert "no failed or interrupted stages in this run\n" in result.stdout
        assert "successful output" not in result.stdout

    def test_reader_neither_waits_for_cache_lock_nor_creates_logs(self, logs_cli: LogsCli) -> None:
        """Observing a build must remain possible while a writer holds the real cache lock."""
        logs_cli.journal("build/example/run")
        before = sorted(path.relative_to(logs_cli.logs) for path in logs_cli.logs.rglob("*"))
        with cache_lock(
            logs_cli.root / ".cache", exclusive=True, command="build", target="example"
        ):
            result = logs_cli.run_logs("list")
        assert (result.returncode) == (0), result.stderr
        assert (len(result.stdout.splitlines()[1:])) == (1)
        assert (sorted(path.relative_to(logs_cli.logs) for path in logs_cli.logs.rglob("*"))) == (
            before
        )

    @pytest.mark.parametrize(
        ("arguments", "status", "error"),
        [
            pytest.param(("show", "missing"), 1, "no matching log run", id="missing-run"),
            pytest.param(("show", "same"), 1, "ambiguous run ID", id="ambiguous-run"),
            pytest.param(
                ("show", "check/same", "--stage", "missing"),
                1,
                "stage not found",
                id="missing-stage",
            ),
            pytest.param(("show", "--tail", "-1"), 2, "non-negative integer", id="negative-tail"),
        ],
    )
    def test_empty_unknown_ambiguous_and_invalid_requests_have_clear_outcomes(
        self, logs_cli: LogsCli, arguments: tuple[str, ...], status: int, error: str
    ) -> None:
        """Empty listing succeeds, missing/ambiguous selections fail and invalid flags return 2."""
        empty = logs_cli.run_logs("list")
        assert (empty.returncode) == (0), empty.stderr
        assert (empty.stdout) == (
            "RUN COMMAND TARGET PROFILE BUILD_TYPE STATUS STARTED DURATION\n"
        )
        assert not (logs_cli.logs.exists())
        logs_cli.journal("check/same")
        logs_cli.journal("test/same")
        result = logs_cli.run_logs(*arguments)
        assert (result.returncode) == (status), result.stderr
        assert (error) in (result.stderr)
        assert ("Traceback") not in (result.stderr)

    def test_follow_delivers_appends_new_stages_and_final_bytes_once(
        self, logs_cli: LogsCli
    ) -> None:
        """Follow binds one run, drains new child stages and stops on recorded completion."""
        relative = "test/live"
        run = logs_cli.journal(relative, status="running", stages=(("first", "running"),))
        (run / "01-first.log").write_text("hidden history\nready-marker\n")
        observed = b""

        def publish_after_ready(process: subprocess.Popen[str], deadline: float) -> None:
            nonlocal observed
            observed = await_output(process, deadline, b"ready-marker\n")
            with (run / "01-first.log").open("a") as stream:
                stream.write("appended-marker\n")
            logs_cli.journal("check/newer", started="2026-01-03T00:00:00+00:00")
            child = logs_cli.journal(
                relative + "/containers/tool",
                parent=relative + "/containers",
                stages=(("next", "success"),),
            )
            (child / "01-next.log").write_text("new-stage-first\nnew-stage-last\n")
            logs_cli.journal(relative, stages=(("first", "success"),))

        result = logs_cli.run_logs("follow", "--tail", "1", while_running=publish_after_ready)
        assert (result.returncode) == (0), result.stderr
        combined = observed.decode() + result.stdout
        for marker in (
            "ready-marker\n",
            "appended-marker\n",
            "new-stage-first\n",
            "new-stage-last\n",
        ):
            assert (combined.count(marker)) == (1), combined
        assert ("hidden history") not in (combined)
        assert ("check/newer") not in (combined)
        assert ("run finished: success") in (combined)

    def test_interrupting_follow_does_not_change_the_observed_run(self, logs_cli: LogsCli) -> None:
        """Ctrl+C stops only the reader with status 130 and leaves the journal untouched."""
        run = logs_cli.journal("test/live", status="running", stages=(("first", "running"),))
        (run / "01-first.log").write_text("ready-marker\n")
        before = (run / "run.json").read_bytes()

        def interrupt_after_ready(process: subprocess.Popen[str], deadline: float) -> None:
            await_output(process, deadline, b"ready-marker\n")
            process.send_signal(signal.SIGINT)

        result = logs_cli.run_logs("follow", while_running=interrupt_after_ready)
        assert (result.returncode) == (130), result.stderr
        assert ((run / "run.json").read_bytes()) == (before)
        assert ("Traceback") not in (result.stderr)
