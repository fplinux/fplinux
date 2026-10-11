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

import pytest
from fplinux_cli.runtime import ssh_transport

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


def test_archive_ready_session_reconnects_and_retains_private_files_without_xdg(
    archive_session: dict[str, Any], monkeypatch: pytest.MonkeyPatch
) -> None:
    """Reacquiring an archive session publishes its own pointer and removes the RAM copy."""
    session = archive_session
    image = Path(session["image"])
    image.write_bytes(b"private personalized payload")
    monkeypatch.setattr(ssh_transport, "_usb_devices", lambda _session: [Path("/usb/phone")])
    monkeypatch.setattr(ssh_transport, "_ncm_interface", lambda _device, _mac: "usb1")
    monkeypatch.setattr(ssh_transport, "_network_ready", lambda _interface, _session: True)
    identity = session["session_id"]
    monkeypatch.setattr(
        ssh_transport,
        "_ssh_argv",
        lambda _session, _command, **_options: [sys.executable, "-c", f"print({identity!r})"],
    )

    ready = ssh_transport.reacquire_bound_session(session)
    ssh_transport.finish_session(session)
    root = Path(session["runtime_root"])
    loaded = ssh_transport.load_current_session("phone", runtime_root=root)

    assert loaded == ready
    assert loaded["interface"] == "usb1"
    assert not image.exists()
    assert Path(session["private_key"]).read_bytes() == b"private\n"
    config = (root / "current/phone.ssh-config").read_text()
    assert 'ControlMaster "no"' in config
    assert 'ControlPersist "no"' in config
    assert "ControlPath none" in config
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


@pytest.fixture
def terminal_shell(
    archive_session: dict[str, Any], tmp_path: Path
) -> Iterator[tuple[int, subprocess.Popen[bytes], Path]]:
    """Launch the real shell consumer on a test-owned PTY with a local SSH peer."""
    state = tmp_path / "shell-session.json"
    state.write_text(json.dumps(archive_session), encoding="utf-8")
    peer_pid = tmp_path / "peer.pid"
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [
            sys.executable,
            str(ROOT / "tests/fixtures/ssh_transport/interactive_consumer.py"),
            str(state),
            str(peer_pid),
        ],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=python_environment(),
        start_new_session=True,
    )
    os.close(slave)
    try:
        yield master, process, peer_pid
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
    terminal_shell: tuple[int, subprocess.Popen[bytes], Path],
) -> None:
    """EOF exits the child shell and leaves the archive runner available to record completion."""
    master, process, peer_pid = terminal_shell
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
    terminal_shell: tuple[int, subprocess.Popen[bytes], Path], signum: signal.Signals
) -> None:
    """External cancellation returns its signal status and leaves no owned SSH process."""
    master, process, peer_pid = terminal_shell
    _read_until(master, b"terminal peer ready")
    os.kill(process.pid, signum)
    _read_until(master, f"consumer returned: {128 + signum}".encode())
    process.wait(timeout=5)

    assert process.returncode == 128 + signum
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
