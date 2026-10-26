# SPDX-License-Identifier: GPL-2.0-only
"""Observe private archive session files and inherited terminal process ownership.

USB and SSH are controlled boundaries. Files, session validation, terminal input
and process cleanup execute production code on the Linux host.
"""

from __future__ import annotations

import contextlib
import json
import os
import pty
import select
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.runtime import ssh_transport
from fplinux_cli.runtime.connection import ConnectionLostError

from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.process import process_state, python_environment
from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from collections.abc import Iterator
    from typing import Any


@pytest.fixture
def archive_session(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> dict[str, Any]:
    """Keep all session files in a private directory without an XDG runtime dependency."""
    root = tmp_path / "data/session"
    root.mkdir(parents=True, mode=0o700)
    session = create_ready_session(root)
    session["runtime_root"] = str(root)
    Path(session["private_key"]).parent.joinpath("session.json").write_text(
        json.dumps(session), encoding="utf-8"
    )
    monkeypatch.delenv("XDG_RUNTIME_DIR", raising=False)
    return session


def test_archive_ready_reconnect_preserves_current_and_parallel_prepared_session_without_xdg(
    archive_session: dict[str, Any], monkeypatch: pytest.MonkeyPatch
) -> None:
    """Reauthentication cannot rewrite saved state or erase another run's prepared credentials."""
    session = archive_session
    root = Path(session["runtime_root"])
    ssh_transport._mark_current(session)  # noqa: SLF001 -- establish a published ready session.
    current = root / "current/phone.json"
    config = root / "current/phone.ssh-config"
    current_before = current.read_bytes()
    config_before = config.read_bytes()
    state = Path(session["private_key"]).parent / "session.json"
    state_before = state.read_bytes()
    candidate = create_ready_session(root, status="prepared", session_id=b"n" * 32)
    Path(candidate["image"]).write_bytes(b"parallel prepared image")
    candidate_directory = Path(candidate["private_key"]).parent
    candidate_before = {path.name: path.read_bytes() for path in candidate_directory.iterdir()}
    image = Path(session["image"])
    image.write_bytes(b"private personalized payload")
    monkeypatch.setattr(ssh_transport, "_usb_devices", lambda _session: [Path("/usb/phone")])
    monkeypatch.setattr(ssh_transport, "_ncm_interface", lambda _device, _mac: "usb1")
    monkeypatch.setattr(ssh_transport, "_network_ready", lambda _interface, _session: True)
    identity = session["session_id"]
    monkeypatch.setattr(
        ssh_transport,
        "_ssh_argv",
        lambda _session, command, **_options: [
            sys.executable,
            "-c",
            f"print({identity!r})"
            if command == "fplinux-session-id"
            else "print('6.18.42-fplinux-9999999999999999')",
        ],
    )

    ready = ssh_transport.reacquire_bound_session(session, expected_device_identity="9" * 64)
    ssh_transport.finish_session(session)
    loaded = ssh_transport.load_saved_session("phone", runtime_root=root)

    assert ready == {**session, "interface": "usb1"}
    assert loaded == session
    assert current.read_bytes() == current_before
    assert config.read_bytes() == config_before
    assert state.read_bytes() == state_before
    assert {
        path.name: path.read_bytes() for path in candidate_directory.iterdir()
    } == candidate_before
    assert not image.exists()
    assert Path(session["private_key"]).read_bytes() == b"private\n"
    configuration = config.read_text()
    assert 'ControlMaster "no"' in configuration
    assert 'ControlPersist "no"' in configuration
    assert "ControlPath none" in configuration
    assert (root / "current/phone.json").stat().st_mode & 0o777 == 0o600
    assert root.stat().st_mode & 0o777 == 0o700


def test_archive_incomplete_session_erases_keys_and_ram_copy_without_xdg(
    archive_session: dict[str, Any],
) -> None:
    """An attempt that never publishes readiness leaves no secret session directory."""
    session = archive_session
    directory = Path(session["private_key"]).parent
    image = Path(session["image"])
    image.write_bytes(b"private personalized payload")
    state = {**session, "status": "prepared"}
    (directory / "session.json").write_text(json.dumps(state), encoding="utf-8")

    ssh_transport.finish_session(session)

    assert not directory.exists()
    assert not image.exists()


@pytest.mark.parametrize(
    ("ssh_status", "usb_status", "disconnected"),
    [
        (255, "present", True),
        (255, "absent", True),
        (0, "unavailable", False),
        (17, "unavailable", False),
    ],
)
def test_archive_shell_recovers_ssh_failure_independently_of_usb_presence(
    archive_session: dict[str, Any],
    monkeypatch: pytest.MonkeyPatch,
    ssh_status: int,
    usb_status: str,
    *,
    disconnected: bool,
) -> None:
    """A completed real child cannot bypass disconnect handling or turn normal exits into it."""
    peer_pid = Path(archive_session["runtime_root"]) / "completed-peer.pid"
    process = subprocess.Popen(
        [
            sys.executable,
            str(ROOT / "tests/fixtures/ssh_transport/interactive_peer.py"),
            str(peer_pid),
            str(ssh_status),
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        stdout, stderr = process.communicate(timeout=5)
    except BaseException:
        process.kill()
        process.communicate()
        raise
    assert process.returncode == ssh_status, (stdout, stderr)

    def usb_devices(
        _session: dict[str, Any], *, require_absence_proof: bool = False
    ) -> list[Path]:
        del require_absence_proof
        if usb_status == "unavailable":
            pytest.fail("a normal SSH exit must not probe USB")
        return [Path("/usb/phone")] if usb_status == "present" else []

    monkeypatch.setattr(ssh_transport, "_usb_devices", usb_devices)
    monkeypatch.setattr(ssh_transport, "_ssh_argv", lambda _session: ["completed-ssh-peer"])
    monkeypatch.setattr(os, "isatty", lambda _descriptor: True)
    with mock.patch.object(subprocess, "Popen", return_value=process):
        if disconnected:
            with pytest.raises(ConnectionLostError):
                ssh_transport.run_shell(archive_session)
        else:
            assert ssh_transport.run_shell(archive_session) == ssh_status

    assert process_state(int(peer_pid.read_text())) is None


@pytest.fixture
def terminal_shell(
    archive_session: dict[str, Any], tmp_path: Path
) -> Iterator[tuple[int, subprocess.Popen[bytes], Path, Path]]:
    """Launch the real shell consumer on a test-owned PTY with a local SSH peer."""
    state = tmp_path / "shell-session.json"
    state.write_text(json.dumps(archive_session), encoding="utf-8")
    peer_pid = tmp_path / "peer.pid"
    usb_state = tmp_path / "usb-state"
    usb_state.write_text("present", encoding="ascii")
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [
            sys.executable,
            str(ROOT / "tests/fixtures/ssh_transport/interactive_consumer.py"),
            str(state),
            str(peer_pid),
            str(usb_state),
        ],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=python_environment(),
        start_new_session=True,
    )
    os.close(slave)
    try:
        yield master, process, peer_pid, usb_state
    finally:
        if process.poll() is None:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        os.close(master)


def _read_until(master: int, expected: bytes) -> bytes:
    """Collect terminal output within a bounded interval."""
    deadline = time.monotonic() + 5
    output = bytearray()
    while expected not in output and time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.05)
        if ready:
            output.extend(os.read(master, 65536))
    assert expected in output, bytes(output)
    return bytes(output)


def test_archive_shell_receives_terminal_input_and_ctrl_d_returns_to_consumer(
    terminal_shell: tuple[int, subprocess.Popen[bytes], Path, Path],
) -> None:
    """EOF exits the child shell and leaves the archive runner available to record completion."""
    master, process, peer_pid, _usb_state = terminal_shell
    _read_until(master, b"terminal peer ready")
    os.write(master, b"hello\n")
    output = _read_until(master, b"peer received: hello")
    os.write(master, b"\x04")
    output += _read_until(master, b"consumer returned: 0")
    process.wait(timeout=5)

    assert process.returncode == 0
    assert b"terminal peer EOF" in output
    assert process_state(int(peer_pid.read_text())) is None


@pytest.mark.parametrize("signum", [signal.SIGINT, signal.SIGTERM, signal.SIGHUP])
def test_archive_shell_cancellation_reaps_ssh_child(
    terminal_shell: tuple[int, subprocess.Popen[bytes], Path, Path], signum: signal.Signals
) -> None:
    """External cancellation returns its signal status and leaves no owned SSH process."""
    master, process, peer_pid, _usb_state = terminal_shell
    _read_until(master, b"terminal peer ready")
    os.kill(process.pid, signum)
    _read_until(master, f"consumer returned: {128 + signum}".encode())
    process.wait(timeout=5)

    assert process.returncode == 128 + signum
    assert process_state(int(peer_pid.read_text())) is None


@pytest.mark.parametrize(
    ("usb_status", "expected_status", "expected_output"),
    [
        ("absent", 75, b"consumer observed USB disconnect"),
        ("ambiguous", 1, b"more than one USB device matches this RAM session"),
        ("unavailable", 1, b"controlled USB discovery failure"),
    ],
)
def test_archive_shell_reaps_waiting_ssh_child_and_distinguishes_usb_failures(
    terminal_shell: tuple[int, subprocess.Popen[bytes], Path, Path],
    usb_status: str,
    expected_status: int,
    expected_output: bytes,
) -> None:
    """A blocked real SSH child stops on exact absence while discovery failures stay errors."""
    master, process, peer_pid, usb_state = terminal_shell
    _read_until(master, b"terminal peer ready")
    usb_state.write_text(usb_status, encoding="ascii")

    _read_until(master, expected_output)
    process.wait(timeout=5)

    assert process.returncode == expected_status
    assert process_state(int(peer_pid.read_text())) is None


def test_archive_stream_hangup_reaps_detached_ssh_child(
    archive_session: dict[str, Any], tmp_path: Path
) -> None:
    """A terminal hangup cannot leave the archive's isolated binary reader running."""
    state = tmp_path / "stream-session.json"
    state.write_text(json.dumps(archive_session), encoding="utf-8")
    directory = tmp_path / "bin"
    directory.mkdir()
    fixture_root = ROOT / "tests/fixtures/ssh_transport"
    install_python_script(fixture_root / "interruptible_stream.py", directory / "ssh")
    peer_pid = tmp_path / "stream.pid"
    destination = tmp_path / "stream.bin"
    environment = python_environment()
    environment["PATH"] = str(directory)
    environment["FPLINUX_STREAM_PID"] = str(peer_pid)
    process = subprocess.Popen(
        [sys.executable, str(fixture_root / "stream_helper.py"), str(state), str(destination)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        start_new_session=True,
    )
    try:
        deadline = time.monotonic() + 5
        while not peer_pid.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.01)
        assert peer_pid.is_file()
        os.kill(process.pid, signal.SIGHUP)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 129, (stdout, stderr)
        assert destination.read_bytes() == b"\x00\xffstream started\n"
        assert process_state(int(peer_pid.read_text())) is None
    finally:
        if process.poll() is None:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        if peer_pid.exists():
            with contextlib.suppress(ProcessLookupError):
                os.killpg(int(peer_pid.read_text()), signal.SIGKILL)
