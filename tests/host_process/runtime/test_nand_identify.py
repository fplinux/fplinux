# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for retained diagnostics around explicit NAND identification."""

from __future__ import annotations

import os
import shlex
import subprocess
import sys
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.runtime import boot, nand_identify, workflow
from fplinux_cli.runtime.bundle import ValidatedBundle
from fplinux_cli.runtime.ssh import transport as ssh_transport

from tests import ROOT
from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from pathlib import Path
    from typing import Any


@pytest.mark.parametrize("audit_available", [True, False], ids=["audit", "older-kernel"])
def test_identify_preserves_geometry_and_records_audit_with_only_one_reader_open(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    *,
    audit_available: bool,
) -> None:
    """A real shell and SSH stream retain snapshots without consuming raw FIFO bytes."""
    raw = tmp_path / "raw nand"
    os.mkfifo(raw)
    geometry = tmp_path / "geometry"
    os.mkfifo(geometry)
    report = "id_bytes=2cda\nchip=UNKNOWN\ngeometry_source=unknown\n"
    audit = tmp_path / "audit"
    if audit_available:
        audit.write_text("state=idle total_commands=0\n", encoding="ascii")
    data, logs, calls = _local_identification(tmp_path, monkeypatch, raw)
    writer = subprocess.Popen(
        [
            sys.executable,
            str(ROOT / "tests/host_process/runtime/fixtures/nand-identify/fifo_writer.py"),
            str(raw),
            str(geometry),
            str(audit),
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        result = workflow.run_phone(tmp_path, data, logs, nand_action="identify")
        stdout, stderr = writer.communicate(timeout=5)
        assert writer.returncode == 0, (stdout, stderr)
    finally:
        if writer.poll() is None:
            writer.kill()
            writer.wait()

    assert result == 0
    assert calls == ["authenticated stream"]
    assert capsys.readouterr().out == report
    retained = next(logs.glob("*/runtime.log")).read_text(encoding="utf-8")
    if audit_available:
        assert "NAND audit before:\nstate=idle total_commands=0\n" in retained
        assert "NAND audit after:\nstate=idle total_commands=2\n" in retained
    else:
        assert "NAND audit before:\nunavailable: cannot read NAND audit\n" in retained
        assert "NAND audit after:\nunavailable: cannot read NAND audit\n" in retained


def test_identify_retains_both_audits_when_reader_open_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """A failing shell redirection still reaches the second snapshot and keeps its error."""
    raw = tmp_path / "missing-raw-nand"
    (tmp_path / "geometry").write_text("geometry must not be read\n", encoding="ascii")
    (tmp_path / "audit").write_text("state=failed last_error=-5\n", encoding="ascii")
    original = subprocess.run(
        [
            "sh",
            "-c",
            f"cat {shlex.quote(str(tmp_path / 'geometry'))} 3<{shlex.quote(str(raw))}",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert original.returncode != 0
    data, logs, calls = _local_identification(tmp_path, monkeypatch, raw)
    result = workflow.run_phone(tmp_path, data, logs, nand_action="identify")

    assert result == 1
    assert calls == ["authenticated stream"]
    output = capsys.readouterr()
    assert output.out == ""
    assert f"exit status {original.returncode}:" in output.err
    assert "missing-raw-nand" in output.err
    retained = next(logs.glob("*/runtime.log")).read_text(encoding="utf-8")
    assert "NAND audit before:\nstate=failed last_error=-5\n" in retained
    assert "NAND audit after:\nstate=failed last_error=-5\n" in retained
    assert "geometry must not be read" not in retained


def _local_identification(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, raw: Path
) -> tuple[Path, Path, list[str]]:
    """Replace the SSH executable while retaining real stream handling and runtime dispatch."""
    runtime = tmp_path / "runtime"
    runtime.mkdir(mode=0o700)
    session = create_ready_session(runtime)
    calls: list[str] = []

    def local_shell(_session: dict[str, Any], command: str) -> list[str]:
        calls.append("authenticated stream")
        attribute_root = f"/sys/class/misc/{raw.name}/device/"
        for name in ("geometry", "audit"):
            command = command.replace(
                shlex.quote(attribute_root + name), shlex.quote(str(tmp_path / name))
            )
        return ["sh", "-c", command]

    monkeypatch.setattr(nand_identify, "GEOMETRY_TIMEOUT_SECONDS", 5)
    monkeypatch.setattr(ssh_transport, "ssh_argv", local_shell)
    monkeypatch.setattr(
        boot, "reconnect_live_bundle", lambda *_args, **_kwargs: (ssh_transport, session)
    )
    logs = tmp_path / "logs"
    logs.mkdir()
    admitted = ValidatedBundle(
        tmp_path,
        {
            "target": "phone",
            "profile": None,
            "build_type": "release",
            "device_data": None,
            "nand": {"raw_device": str(raw)},
        },
        "a" * 64,
        "b" * 64,
        None,
    )
    monkeypatch.setattr(workflow, "admit_bundle", lambda _bundle: admitted)
    return runtime, logs, calls
