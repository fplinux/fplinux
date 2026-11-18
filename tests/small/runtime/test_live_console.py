# SPDX-License-Identifier: GPL-2.0-only
"""Selection and remote-action policy for retained phone connections."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any

import pytest
from fplinux_cli.runtime import boot, live
from fplinux_cli.runtime.bundle import ValidatedBundle
from fplinux_cli.runtime.connection import ConnectionLostError
from fplinux_cli.runtime.ssh import transport as ssh_transport
from fplinux_cli.runtime.state import exclusive_lock

from tests.ssh_transport_support import create_ready_session


class Phone:
    """Controlled USB and SSH boundary for a saved session."""

    def __init__(self, *, present: bool = True) -> None:
        """Choose whether this saved USB endpoint is present."""
        self.present = present
        self.authenticated: list[str | None] = []
        self.commands: list[str] = []
        self.shells = 0

    def usb_present(self, _session: dict[str, Any]) -> bool:
        """Expose the controlled USB state."""
        return self.present

    def reacquire_bound_session(
        self, session: dict[str, Any], *, expected_device_identity: str | None
    ) -> dict[str, Any]:
        """Retain the identity used to authenticate the selected session."""
        self.authenticated.append(expected_device_identity)
        return session

    def run_remote(
        self, _session: dict[str, Any], command: str, **_options: object
    ) -> subprocess.CompletedProcess[str]:
        """Return metadata and simulate a failed one-shot action."""
        self.commands.append(command)
        if command.startswith("uname -r;"):
            return subprocess.CompletedProcess(command, 0, "6.18.42-old\nold-build\n", "")
        return subprocess.CompletedProcess(command, 255)

    def run_shell(self, _session: dict[str, Any]) -> int:
        """Simulate one USB interruption followed by a normal shell exit."""
        self.shells += 1
        if self.shells == 1:
            raise ConnectionLostError
        return 0


def test_select_connection_uses_only_connected_usb_and_rejects_ambiguity() -> None:
    disconnected, connected, other = Phone(present=False), Phone(), Phone()
    first = (disconnected, {"target": "old-phone"})
    second = (connected, {"target": "phone"})
    third = (other, {"target": "other"})

    assert live.select_connection([first, second]) == second
    with pytest.raises(SystemExit, match="No known live phone"):
        live.select_connection([first])
    with pytest.raises(SystemExit, match="other, phone"):
        live.select_connection([first, second, third])


def test_diagnostic_reconnect_rejects_another_ram_session(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    session = create_ready_session(tmp_path / "session")
    monkeypatch.setattr(ssh_transport, "_usb_devices", lambda *_args, **_kwargs: [Path("/usb")])
    monkeypatch.setattr(ssh_transport, "_ncm_interface", lambda *_args: "usb0")
    monkeypatch.setattr(ssh_transport, "_network_ready", lambda *_args: True)
    monkeypatch.setattr(ssh_transport, "ssh_argv", lambda *_args, **_kwargs: ["ssh"])
    monkeypatch.setattr(
        subprocess,
        "run",
        lambda *_args, **_kwargs: subprocess.CompletedProcess([], 0, "different-session\n", ""),
    )

    with pytest.raises(SystemExit, match="different RAM session identity"):
        ssh_transport.reacquire_bound_session(session, expected_device_identity=None)


def test_diagnostic_command_runs_once_and_reports_actual_build(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    phone = Phone()
    result = live.console_connection(
        (phone, {"target": "phone"}),
        tmp_path / "data",
        tmp_path / "logs",
        admitted=None,
        exec_command="printf sent",
    )

    assert result == 255
    assert phone.authenticated == [None]
    assert phone.commands == [
        "uname -r; sed -n 's/^BUILD_ID=//p' /etc/os-release",
        "printf sent",
    ]
    output = capsys.readouterr()
    assert output.out == ""
    assert "phone; kernel: 6.18.42-old; BUILD_ID: old-build" in output.err
    run_dir = next((tmp_path / "logs").iterdir())
    assert json.loads((run_dir / "run.json").read_text())["status"] == "failed"


def test_shell_requires_launcher_lock_but_one_shot_command_does_not(tmp_path: Path) -> None:
    data = tmp_path / "data"
    data.mkdir(mode=0o700)
    phone = Phone()
    connection = (phone, {"target": "phone"})
    with exclusive_lock(data / ".launcher.lock", message="already open"):
        shell_status = live.console_connection(
            connection, data, tmp_path / "shell-logs", admitted=None
        )
        command_status = live.console_connection(
            connection, data, tmp_path / "command-logs", admitted=None, exec_command="false"
        )

    assert shell_status == 1
    assert command_status == 255
    assert phone.shells == 0
    assert phone.authenticated == [None]


def test_archive_manifest_read_error_is_reported_without_traceback(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setattr(live, "host_preflight", lambda: None)

    def unreadable(_path: Path) -> dict[str, Any]:
        message = "manifest unavailable"
        raise OSError(message)

    monkeypatch.setattr(live, "load_runtime_manifest", unreadable)
    result = live.console_bundle(tmp_path, tmp_path / "data", tmp_path / "logs")

    assert result == 1
    output = capsys.readouterr()
    assert "FPLinux: manifest unavailable" in output.err
    assert "Traceback" not in output.err
    run_dir = next((tmp_path / "logs").iterdir())
    assert "manifest unavailable" in (run_dir / "runtime.log").read_text()
    assert json.loads((run_dir / "run.json").read_text())["status"] == "failed"
    assert not (tmp_path / "runtime.log").exists()


def test_strict_rejection_reports_actual_build_without_running_remote_command(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    phone = Phone()
    admitted = ValidatedBundle(
        tmp_path, {"target": "phone", "device_data": None}, "a" * 64, "b" * 64, None
    )
    monkeypatch.setattr(live, "_prepared_identity", lambda *_args: None)

    def reject(*_args: object, **_kwargs: object) -> None:
        message = "different build"
        raise SystemExit(message)

    monkeypatch.setattr(boot, "reconnect_saved_connection", reject)
    result = live.console_connection(
        (phone, {"target": "phone"}),
        tmp_path / "data",
        tmp_path / "logs",
        admitted=admitted,
        exec_command="printf sent",
    )

    assert result == 1
    assert phone.commands == ["uname -r; sed -n 's/^BUILD_ID=//p' /etc/os-release"]


def test_strict_shell_rechecks_source_and_phone_data_before_replug(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    phone = Phone()
    admitted = ValidatedBundle(
        tmp_path, {"target": "phone", "device_data": None}, "a" * 64, "b" * 64, None
    )
    identities = iter(["old-data", "changed-data"])
    revalidations: list[str] = []
    monkeypatch.setattr(live, "_prepared_identity", lambda *_args: next(identities))
    monkeypatch.setattr(
        boot, "reconnect_saved_connection", lambda _admitted, connection, **_kwargs: connection
    )
    result = live.console_connection(
        (phone, {"target": "phone"}),
        tmp_path / "data",
        tmp_path / "logs",
        admitted=admitted,
        revalidate=lambda: revalidations.append("checked"),
    )

    assert result == 1
    assert revalidations == ["checked", "checked"]
    assert phone.shells == 1
    assert phone.commands == ["uname -r; sed -n 's/^BUILD_ID=//p' /etc/os-release"]
