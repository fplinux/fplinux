# SPDX-License-Identifier: GPL-2.0-only
"""Kern memory diagnostics from real Stage subprocess logs without memory pressure."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.common import ROOT
from fplinux_cli.environment.kern import report_kern_memory_failure
from fplinux_cli.reporting.run import RunReporter

_WORKLOAD = ROOT / "tests/host_process/quality/fixtures/kern-memory/failed_workload.py"


@pytest.mark.parametrize(
    ("mode", "has_hint"),
    [
        pytest.param("exact", True, id="confirmed-memory-limit"),
        pytest.param("plain", False, id="unexplained-exit-137"),
        pytest.param("generic", False, id="generic-memory-diagnostics"),
        pytest.param("split", True, id="split-marker-before-large-teardown"),
    ],
)
def test_stage_memory_hint_requires_confirmed_limit_and_preserves_exit(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    mode: str,
    *,
    has_hint: bool,
) -> None:
    """Retain the workload's exit and emit the same confirmed hint to log and stderr."""
    reporter = RunReporter("check", tmp_path / "run", "test", verbose=False)
    stage = reporter.stage("workload", show_tail=False)

    def run_workload() -> None:
        with stage:
            try:
                stage.run(
                    [sys.executable, str(_WORKLOAD), mode, str(stage.log_path), "137"], timeout=5
                )
            except SystemExit:
                report_kern_memory_failure(
                    stage, memory_mib=4096, advice="Use a smaller workload."
                )
                raise

    with pytest.raises(SystemExit) as raised:
        run_workload()

    assert raised.value.code == 137
    hint = "fplinux: Kern memory limit reached (4096 MiB). Use a smaller workload.\n"
    output = capsys.readouterr()
    log = stage.log_path.read_bytes()
    assert output.out == ""
    assert output.err.count(hint) == int(has_hint)
    assert log.count(hint.encode()) == int(has_hint)
    metadata = json.loads(reporter.metadata_path.read_text())
    assert metadata["status"] == "failed"
    assert metadata["stages"][0]["exit"] == 137
    if mode == "split":
        marker_offset = log.index(b"kern: the workload was killed")
        assert marker_offset % 65536 == 65519
        assert len(log) - marker_offset > 131072


def test_unreadable_memory_diagnostic_log_preserves_workload_exit(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """An optional diagnostic read cannot replace the original subprocess failure."""
    reporter = RunReporter("check", tmp_path / "run", "test", verbose=False)
    stage = reporter.stage("workload", show_tail=False)

    def run_workload() -> None:
        with stage:
            try:
                stage.run(
                    [sys.executable, str(_WORKLOAD), "exact", str(stage.log_path), "137"],
                    timeout=5,
                )
            except SystemExit:
                with mock.patch.object(
                    Path, "open", side_effect=PermissionError("unreadable log")
                ):
                    report_kern_memory_failure(stage, memory_mib=4096, advice="")
                raise

    with pytest.raises(SystemExit) as raised:
        run_workload()

    assert raised.value.code == 137
    assert "Kern memory limit reached" not in capsys.readouterr().err
    assert b"Kern memory limit reached" not in stage.log_path.read_bytes()
