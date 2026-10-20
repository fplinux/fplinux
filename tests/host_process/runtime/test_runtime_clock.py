# SPDX-License-Identifier: GPL-2.0-only
"""Fresh-session clock policy through real Linux and Windows SSH child boundaries."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.runtime import boot, ssh_transport, workflow
from fplinux_cli.runtime.bundle import ValidatedBundle

from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.ssh_transport_support import create_ready_session
from tests.windows_host_support import endpoint_session, native_process_transport

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize("transport_kind", ["linux", "windows"])
@pytest.mark.parametrize(
    ("reply", "expected"),
    [
        pytest.param(
            {"status": 0, "stdout": "system=1788739201 rtc=kept\n", "stderr": ""},
            "Phone clock set to 2026-09-07T00:00:01Z (RTC kept).",
            id="rtc-kept",
        ),
        pytest.param(
            {"status": 0, "stdout": "system=1788739201 rtc=written\n", "stderr": ""},
            "Phone clock set to 2026-09-07T00:00:01Z (RTC written).",
            id="rtc-written",
        ),
        pytest.param(
            {
                "status": 1,
                "stdout": "system=1788739200 rtc=failed\n",
                "stderr": "fplinux-clock: cannot store a readable time in /dev/rtc0\n",
            },
            "fplinux ssh: phone clock set to 2026-09-07T00:00:00Z, but its RTC was not "
            "written: fplinux-clock: cannot store a readable time in /dev/rtc0",
            id="rtc-not-written",
        ),
        pytest.param(
            {"status": 255, "stdout": "", "stderr": "Connection closed by 10.23.45.2 port 22\n"},
            "fplinux ssh: phone clock was not set: Connection closed by 10.23.45.2 port 22",
            id="transport-lost",
        ),
        pytest.param(
            {"status": 0, "stdout": "done\n", "stderr": ""},
            "fplinux ssh: phone clock was not set: unexpected clock status (exit 0)",
            id="unexpected-status",
        ),
    ],
)
def test_fresh_boot_reports_clock_result_and_reconnect_preserves_clock_and_command_stdout(  # noqa: PLR0913
    *,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capfd: pytest.CaptureFixture[str],
    transport_kind: str,
    reply: dict[str, Any],
    expected: str,
) -> None:
    """Boot/discovery are controlled; both selected transports execute the real local peer."""
    peer = tmp_path / "clock-peer"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/runtime-clock/clock_peer.py", peer
    )
    connection: tuple[Any, dict[str, Any]]
    if transport_kind == "linux":
        root = tmp_path / "session"
        root.mkdir(mode=0o700)
        connection = (ssh_transport, create_ready_session(root))
        monkeypatch.setattr(ssh_transport, "_require_tool", lambda _name: str(peer))
    else:
        transport = native_process_transport(tmp_path)
        transport.bundle.tools["ssh"] = peer
        connection = (transport, endpoint_session(tmp_path / "session"))
    response = tmp_path / "response.json"
    response.write_text(json.dumps(reply))
    commands = tmp_path / "commands"
    monkeypatch.setenv("FPLINUX_TEST_CLOCK_RESPONSE", str(response))
    monkeypatch.setenv("FPLINUX_TEST_CLOCK_COMMANDS", str(commands))
    monkeypatch.setattr("fplinux_cli.runtime.workflow.time.time", lambda: 1788739200.75)
    runtime = {
        "identity": {"target": {"display_name": "Test phone"}},
        "device_data": None,
        "target": "phone",
        "profile": None,
        "build_type": "release",
        "transport": "usb-ncm",
    }
    admitted = ValidatedBundle(tmp_path, runtime, "a" * 64, "b" * 64, None)
    monkeypatch.setattr(workflow, "admit_bundle", lambda _bundle: admitted)
    loaded = False

    def start_phone(*_arguments: object, **_options: object) -> tuple[Any, dict[str, Any]]:
        nonlocal loaded
        assert not loaded
        loaded = True
        return connection

    def saved_phone(*_arguments: object, **_options: object) -> tuple[Any, dict[str, Any]] | None:
        return connection if loaded else None

    monkeypatch.setattr(boot, "run_bundle", start_phone)
    monkeypatch.setattr(boot, "reconnect_live_bundle", saved_phone)
    monkeypatch.setattr("fplinux_cli.runtime.workflow.os.isatty", lambda _descriptor: False)
    data = tmp_path / "data"
    logs = tmp_path / "logs"

    assert workflow.run_phone(tmp_path, data, logs) == 0
    fresh_output = capfd.readouterr()
    assert fresh_output.out.count(expected) == 1
    assert fresh_output.err == ""
    assert (
        workflow.run_phone(tmp_path, data, logs, reconnect=True, exec_command="printf ready") == 0
    )

    output = capfd.readouterr()
    assert output.out == "ready"
    assert expected not in output.err
    assert commands.read_text().splitlines() == ["fplinux-clock 1788739200", "printf ready"]
    runtime_logs = sorted(logs.glob("*/runtime.log"))
    assert len(runtime_logs) == 2
    assert expected in runtime_logs[0].read_text()
    assert "phone clock" not in runtime_logs[1].read_text().lower()
