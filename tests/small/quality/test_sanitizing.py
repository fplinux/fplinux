# SPDX-License-Identifier: GPL-2.0-only
"""Sanitizer findings remain command failures independently of pytest's outcome."""

from __future__ import annotations

from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.quality import runtime, testing
from fplinux_cli.reporting.run import RunReporter, Stage

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize("pytest_exit", [0, 1])
@pytest.mark.parametrize(
    "diagnostic",
    [
        b"ERROR: AddressSanitizer: heap-use-after-free\nallocation trace\n",
        b"runtime error: signed integer overflow\noperand details\n",
    ],
)
def test_findings_fail_even_when_pytest_accepts_child_failure_and_retain_complete_report(
    tmp_path: Path, pytest_exit: int, diagnostic: bytes
) -> None:
    """The external runtime may pass or fail; its full finding still reaches the public log."""
    reporter = RunReporter("test", tmp_path / "logs", "test", verbose=False)
    report = reporter.root / "containers/selected/sanitizer/report.child.12"

    def kern_process(_stage: Stage, _command: list[str], **_options: object) -> None:
        report.write_bytes(diagnostic)
        if pytest_exit:
            raise SystemExit(pytest_exit)

    with (
        mock.patch.object(runtime, "kern_environment", return_value={}),
        mock.patch.object(Stage, "run", autospec=True, side_effect=kern_process),
        pytest.raises(SystemExit) as raised,
        reporter.stage("selected") as stage,
    ):
        runtime.run_quality_command(
            stage,
            tmp_path / "workspace",
            ["python3", "-m", "pytest"],
            kern="kern",
            rootfs=tmp_path / "rootfs",
            sanitize=True,
        )

    assert raised.value.code == (
        1 if pytest_exit else "fplinux: native sanitizer reported 1 finding(s)"
    )
    assert report.read_bytes() == diagnostic
    assert diagnostic in stage.log_path.read_bytes()


def test_benign_sanitizer_diagnostics_do_not_fail_a_passing_workload(tmp_path: Path) -> None:
    """A runtime note is retained without turning report-file existence into an error."""
    reporter = RunReporter("test", tmp_path / "logs", "test", verbose=False)
    report = reporter.root / "containers/selected/sanitizer/report.child.12"

    def kern_process(_stage: Stage, _command: list[str], **_options: object) -> None:
        report.write_text("AddressSanitizer Init done\n")

    with (
        mock.patch.object(runtime, "kern_environment", return_value={}),
        mock.patch.object(Stage, "run", autospec=True, side_effect=kern_process) as process,
        reporter.stage("selected") as stage,
    ):
        runtime.run_quality_command(
            stage,
            tmp_path / "workspace",
            ["python3", "-m", "pytest"],
            kern="kern",
            rootfs=tmp_path / "rootfs",
            sanitize=True,
        )

    command = process.call_args.args[1]
    assert command[command.index("--memory") + 1] == "3072m"
    assert command[command.index("--tmpfs") + 1] == "/tmp:2g"  # noqa: S108 -- container tmpfs.
    assert report.read_text() == "AddressSanitizer Init done\n"


def test_sanitizer_workloads_share_each_selected_tier_budget_once() -> None:
    """Native instrumentation has separate declared budgets without changing ordinary tests."""
    names = [
        "tests.host_tool.gui.test_ui_frame",
        "tests.host_tool.gui.test_ui_lists",
        "tests.host_process.quality.test_test_runner",
    ]
    [(_label, _command, timeout)] = testing.pytest_commands(names, sanitize=True)
    [(_label, _command, ordinary)] = testing.pytest_commands(names)
    assert timeout == 3150
    assert ordinary == 2100
