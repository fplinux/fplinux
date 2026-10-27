# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real SSH child lifetime and bytes with a replaced Windows discovery boundary."""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import signal
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING, Any, override

import pytest
from fplinux_cli.device_data.cpio import create_device_data_initramfs
from fplinux_cli.runtime import state, windows_host
from fplinux_cli.runtime.connection import ConnectionLostError, SessionResumedError

from tests import ROOT
from tests.fdt import binary_tree
from tests.fixtures.executables import install_python_script
from tests.fixtures.portable_archive import capsule_files, refresh_archive
from tests.process import python_environment
from tests.windows_host_support import (
    StubWindowsHost,
    admitted_windows,
    archive_files,
    endpoint_session,
    endpoint_snapshot,
    native_process_transport,
    saved_connection,
    session_preparation_bundle,
)

if TYPE_CHECKING:
    from collections.abc import Callable


@pytest.mark.parametrize(
    ("probe", "capture_output"),
    [("authentication", False), ("metadata", False), ("both", True)],
    ids=["authentication", "metadata", "combined-captured-output"],
)
def test_metadata_probes_preserve_binary_stdin_for_remote_command(
    tmp_path: Path, probe: str, *, capture_output: bool
) -> None:
    """Real host children retain command input; native Windows discovery and ACLs are stubs."""
    session = endpoint_session(tmp_path / "session")
    state_path = tmp_path / "session.json"
    state_path.write_text(json.dumps(session), encoding="utf-8")
    output = tmp_path / "received.bin"
    payload = bytes(range(256)) * 256

    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "tests.fixtures.ssh_transport.windows_stdin_consumer",
            str(state_path),
            probe,
            "capture" if capture_output else "inherit",
        ],
        input=payload,
        capture_output=True,
        cwd=ROOT,
        env={
            **python_environment(),
            "FPLINUX_TEST_SESSION_ID": "ab" * 32,
            "FPLINUX_TEST_KERNEL": "6.12.0-fplinux-1212121212121212",
            "FPLINUX_TEST_STDIN_OUTPUT": str(output),
        },
        timeout=10,
        check=False,
        creationflags=windows_host._console_creation_flags(),  # noqa: SLF001
    )

    assert result.returncode == 0, result.stderr.decode(errors="replace")
    assert result.stdout == b"stdin received\n"
    assert output.read_bytes() == payload


@pytest.fixture(autouse=True)
def windows_console_boundary(monkeypatch: pytest.MonkeyPatch) -> None:
    """Check Windows console flags, then execute the process on the Linux test host."""
    native_popen = subprocess.Popen
    monkeypatch.setattr(subprocess, "CREATE_NO_WINDOW", 0x08000000, raising=False)

    def launch(
        *arguments: Any,  # noqa: ANN401 -- transparent subprocess forwarding.
        **options: Any,  # noqa: ANN401 -- transparent subprocess forwarding.
    ) -> subprocess.Popen[Any]:
        expected = 0 if "-tt" in arguments[0] else 0x08000000
        assert options.pop("creationflags", 0) == expected
        return native_popen(*arguments, **options, creationflags=0)

    monkeypatch.setattr(subprocess, "Popen", launch)


@dataclass(frozen=True)
class WindowsAclBoundary:
    """The production host and JSON state replacing unavailable NTFS APIs."""

    host: windows_host.WindowsHost
    path: Path
    state: Path


@pytest.fixture
def windows_acl_processes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> Callable[[Path, dict[str, Any]], WindowsAclBoundary]:
    """Lend real processes and one ACL record in place of native Windows APIs."""
    state = tmp_path / "acl.json"
    sid = "S-1-5-21-100-200-300-1001"
    monkeypatch.setenv("FPLINUX_TEST_ACL_STATE", str(state))
    icacls = tmp_path / "icacls.exe"
    powershell = tmp_path / "powershell.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_icacls_boundary.py",
        icacls,
    )
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_acl_boundary.py",
        powershell,
    )
    host = windows_host.WindowsHost.__new__(windows_host.WindowsHost)
    host.sid = sid
    host.icacls = icacls
    host.powershell = powershell

    def bind(path: Path, acl: dict[str, Any]) -> WindowsAclBoundary:
        state.write_text(json.dumps(acl), encoding="utf-8")
        return WindowsAclBoundary(host, path, state)

    return bind


@pytest.fixture
def explicit_public_key_acl(
    tmp_path: Path,
    windows_acl_processes: Callable[[Path, dict[str, Any]], WindowsAclBoundary],
) -> WindowsAclBoundary:
    """Model ssh-keygen's explicit Everyone grant using real boundary processes."""
    path = tmp_path / "client-key.pub"
    path.write_text("controlled public key\n", encoding="ascii")
    return windows_acl_processes(
        path,
        {
            "owner": "S-1-5-21-100-200-300-1001",
            "protected": True,
            "rules": [
                {"sid": trustee, "type": "Allow", "inherited": False}
                for trustee in (
                    "S-1-1-0",
                    "S-1-5-21-100-200-300-1001",
                    "S-1-5-18",
                    "S-1-5-32-544",
                )
            ],
        },
    )


@pytest.mark.parametrize("entry", ["host", "workflow"])
@pytest.mark.parametrize("existing", [False, True], ids=["new", "existing"])
def test_runtime_directory_protects_inherited_access_and_keeps_data_on_reopen(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    windows_acl_processes: Callable[[Path, dict[str, Any]], WindowsAclBoundary],
    entry: str,
    *,
    existing: bool,
) -> None:
    """Both runtime entries protect account-owned directories through simulated NTFS APIs."""
    path = tmp_path / "data"
    if existing:
        path.mkdir()
        (path / "retained.txt").write_bytes(b"existing phone data\n")
    boundary = windows_acl_processes(
        path,
        {
            "owner": "S-1-5-21-100-200-300-1001",
            "protected": False,
            "rules": [
                {"sid": trustee, "type": "Allow", "inherited": True}
                for trustee in ("S-1-5-32-544", "S-1-5-18", "S-1-5-11", "S-1-5-32-545")
            ],
        },
    )
    open_directory: Callable[[Path], Path] = boundary.host.runtime_root
    if entry == "workflow":
        monkeypatch.setattr(state, "os", SimpleNamespace(name="nt"))
        monkeypatch.setattr(windows_host, "WindowsHost", lambda: boundary.host)
        open_directory = state.private_directory

    assert open_directory(path) == path
    acl = json.loads(boundary.state.read_text(encoding="utf-8"))
    assert acl["protected"] is True
    assert acl["owner"] == "S-1-5-21-100-200-300-1001"
    assert {rule["sid"] for rule in acl["rules"]} == {
        "S-1-5-21-100-200-300-1001",
        "S-1-5-18",
        "S-1-5-32-544",
    }
    boundary.host.require_private(path)
    if not existing:
        (path / "retained.txt").write_bytes(b"existing phone data\n")
    assert open_directory(path) == path
    assert (path / "retained.txt").read_bytes() == b"existing phone data\n"


@pytest.mark.parametrize("violation", ["owner", "explicit-allow", "deny"])
def test_runtime_directory_does_not_admit_foreign_ownership_or_explicit_access(
    tmp_path: Path,
    windows_acl_processes: Callable[[Path, dict[str, Any]], WindowsAclBoundary],
    violation: str,
) -> None:
    """Protecting a directory does not take ownership or accept another account's access."""
    path = tmp_path / "data"
    path.mkdir()
    other = "S-1-5-21-900-901-902-1002"
    acl: dict[str, Any] = {
        "owner": other if violation == "owner" else "S-1-5-21-100-200-300-1001",
        "protected": False,
        "rules": [
            {
                "sid": other,
                "type": "Deny" if violation == "deny" else "Allow",
                "inherited": False,
            }
        ],
    }
    boundary = windows_acl_processes(path, acl)
    before = boundary.state.read_bytes()
    error = "another Windows account" if violation == "owner" else "Session path is not private"

    with pytest.raises(SystemExit, match=error):
        boundary.host.runtime_root(path)

    if violation == "owner":
        assert boundary.state.read_bytes() == before
    else:
        protected = json.loads(boundary.state.read_text(encoding="utf-8"))
        assert acl["rules"][0] in protected["rules"]
        with pytest.raises(SystemExit, match="Session path is not private"):
            boundary.host.require_private(path)


def test_new_public_key_loses_explicit_everyone_access_before_private_validation(
    explicit_public_key_acl: WindowsAclBoundary,
) -> None:
    """Production protection removes a public grant through simulated Windows APIs."""
    boundary = explicit_public_key_acl

    boundary.host.secure_new(boundary.path)

    acl = json.loads(boundary.state.read_text(encoding="utf-8"))
    assert acl["protected"] is True
    assert acl["owner"] == "S-1-5-21-100-200-300-1001"
    assert {rule["sid"] for rule in acl["rules"]} == {
        "S-1-5-21-100-200-300-1001",
        "S-1-5-18",
        "S-1-5-32-544",
    }
    boundary.host.require_private(boundary.path)


def test_new_item_with_another_account_grant_remains_rejected(
    explicit_public_key_acl: WindowsAclBoundary,
) -> None:
    """Removing Everyone does not admit an unrelated account's explicit grant."""
    boundary = explicit_public_key_acl
    acl = json.loads(boundary.state.read_text(encoding="utf-8"))
    acl["rules"].append({"sid": "S-1-5-21-900-901-902-1002", "type": "Allow", "inherited": False})
    boundary.state.write_text(json.dumps(acl), encoding="utf-8")

    with pytest.raises(SystemExit, match="Session path is not private"):
        boundary.host.secure_new(boundary.path)

    protected = json.loads(boundary.state.read_text(encoding="utf-8"))
    trustees = {rule["sid"] for rule in protected["rules"]}
    assert "S-1-1-0" not in trustees
    assert "S-1-5-21-900-901-902-1002" in trustees
    with pytest.raises(SystemExit, match="Session path is not private"):
        boundary.host.require_private(boundary.path)


def test_binary_stream_preserves_bytes_and_excludes_stderr(tmp_path: Path) -> None:
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    output = tmp_path / "nand.bin"
    with output.open("w+b") as destination:
        transport.stream_remote(session, "binary", destination, timeout=3)
    assert output.read_bytes() == b"\x00\r\n\xff\x80"


def test_binary_stream_progress_reports_written_bytes(tmp_path: Path) -> None:
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    observed: list[int] = []
    output = tmp_path / "nand.bin"

    with output.open("w+b") as destination:
        transport.stream_remote(
            session, "binary", destination, timeout=3, progress=observed.append
        )

    assert observed[-1] == 5
    assert output.read_bytes() == b"\x00\r\n\xff\x80"


def test_binary_stream_progress_counts_overwrites_without_counting_the_existing_tail(
    tmp_path: Path,
) -> None:
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    observed: list[int] = []
    output = tmp_path / "existing.bin"
    output.write_bytes(b"keepabcdefgh")

    with output.open("r+b") as destination:
        destination.seek(4)
        transport.stream_remote(
            session, "binary", destination, timeout=3, progress=observed.append
        )

    assert observed[-1] == 5
    assert output.read_bytes() == b"keep\x00\r\n\xff\x80fgh"


@pytest.mark.parametrize("status", [0, 7, 130])
def test_interactive_shell_inherits_terminal_output_and_preserves_exit_status(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capfd: pytest.CaptureFixture[str], status: int
) -> None:
    """The SSH process is real; its remote peer and terminal admission are controlled."""
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    monkeypatch.setattr(os, "isatty", lambda _descriptor: True)
    monkeypatch.setenv("FPLINUX_TEST_SHELL_STATUS", str(status))

    assert transport.run_shell(session) == status

    captured = capfd.readouterr()
    assert captured.out == "controlled shell output\n"
    assert captured.err == "controlled shell diagnostic\n"


@pytest.mark.parametrize("present", [False, True])
def test_interactive_ssh_connection_failure_signals_loss_with_any_usb_snapshot(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, *, present: bool
) -> None:
    """A completed real SSH child signals loss even while PnP still reports the phone."""
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    host = transport.host
    assert isinstance(host, StubWindowsHost)
    host.snapshot["devices"] = (
        [{"id": "USB\\VID_0525&PID_A4A6\\0123456789abcdef0123456789abcdef"}] if present else []
    )
    monkeypatch.setattr(os, "isatty", lambda _descriptor: True)
    monkeypatch.setenv("FPLINUX_TEST_SHELL_STATUS", "255")
    native_popen = subprocess.Popen

    def completed_ssh(
        *arguments: Any,  # noqa: ANN401 -- preserve the real subprocess launch boundary.
        **options: Any,  # noqa: ANN401 -- preserve the real subprocess launch boundary.
    ) -> subprocess.Popen[Any]:
        process = native_popen(*arguments, **options)
        try:
            assert process.wait(timeout=5) == 255
        except BaseException:
            if process.poll() is None:
                process.kill()
                process.wait()
            raise
        return process

    monkeypatch.setattr(subprocess, "Popen", completed_ssh)

    with pytest.raises(ConnectionLostError):
        transport.run_shell(session)


def test_interactive_shell_cancellation_reaps_the_owned_child(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Interrupt the Python owner while its real SSH stand-in waits on the terminal."""
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    pid_file = tmp_path / "shell.pid"
    monkeypatch.setattr(os, "isatty", lambda _descriptor: True)
    monkeypatch.setenv("FPLINUX_TEST_SHELL_PID", str(pid_file))
    cancelled = threading.Event()

    def cancel_after_launch() -> None:
        deadline = time.monotonic() + 3
        while not cancelled.is_set() and time.monotonic() < deadline:
            if pid_file.exists():
                os.kill(os.getpid(), signal.SIGINT)
                return
            cancelled.wait(0.01)

    trigger = threading.Thread(target=cancel_after_launch)
    trigger.start()
    try:
        with pytest.raises(KeyboardInterrupt):
            transport.run_shell(session)
    finally:
        cancelled.set()
        trigger.join()

    assert not (Path("/proc") / pid_file.read_text()).exists()


def test_interactive_shell_usb_absence_reaps_ssh_with_stale_network_state(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An unchanged DHCP snapshot cannot keep a disconnected phone's SSH child alive."""
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    host = transport.host
    assert isinstance(host, StubWindowsHost)
    host.snapshot["devices"] = []
    pid_file = tmp_path / "shell.pid"
    monkeypatch.setattr(os, "isatty", lambda _descriptor: True)
    monkeypatch.setenv("FPLINUX_TEST_SHELL_PID", str(pid_file))

    with pytest.raises(ConnectionLostError):
        transport.run_shell(session)

    assert not (Path("/proc") / pid_file.read_text()).exists()


def test_cancelled_usb_presence_probe_reaps_its_owned_powershell_child(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Cancelling a slow read-only PnP query does not wait for its ordinary 20s deadline."""
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    host = windows_host.WindowsHost.__new__(windows_host.WindowsHost)
    host.powershell = tmp_path / "powershell.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_discovery_timeout.py",
        host.powershell,
    )
    transport.host = host
    pid_file = tmp_path / "discovery.pid"
    monkeypatch.setenv("FPLINUX_TEST_DISCOVERY_PID", str(pid_file))

    assert transport.usb_present(session, cancelled=pid_file.exists) is False

    assert not (Path("/proc") / pid_file.read_text()).exists()


def test_remote_command_keeps_captured_output_without_a_console(tmp_path: Path) -> None:
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")

    result = transport.run_remote(session, "failure", capture_output=True)

    assert result.returncode == 7
    assert result.stdout == "partial"
    assert result.stderr == "reader failed\n"


def test_loader_keeps_diagnostics_without_a_console(tmp_path: Path) -> None:
    loader = tmp_path / "loader.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_loader_output.py", loader
    )
    host = StubWindowsHost({})
    log = tmp_path / "loader.log"

    host.run_tool([str(loader)], timeout=3, cwd=tmp_path, log=log)

    assert log.read_text() == "controlled loader output\ncontrolled loader diagnostic\n"


def test_loader_debug_environment_is_local_to_the_owned_child(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    loader = tmp_path / "loader.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_loader_output.py", loader
    )
    captured = tmp_path / "environment.json"
    monkeypatch.setenv("FPLINUX_TEST_LOADER_ENVIRONMENT", str(captured))
    monkeypatch.delenv("LIBUSB_DEBUG", raising=False)
    host = StubWindowsHost({})

    host.run_tool(
        [str(loader)],
        timeout=3,
        cwd=tmp_path,
        log=tmp_path / "loader.log",
        environment={"LIBUSB_DEBUG": "4"},
    )

    assert json.loads(captured.read_text()) == {"LIBUSB_DEBUG": "4"}
    assert "LIBUSB_DEBUG" not in os.environ


@pytest.mark.parametrize(
    "scenario", ["ready", "ready-crlf", "driver-needed", "missing-ready", "transfer-error"]
)
def test_native_usb_progress_belongs_to_the_current_child_and_precedes_transfer(
    tmp_path: Path, scenario: str
) -> None:
    loader = tmp_path / "loader.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/usb_loader.py", loader
    )
    log = tmp_path / "runtime.log"
    log.write_text("FPLINUX_BOOTROM_READY\nprevious invocation\n")
    host = StubWindowsHost({})
    notifications: list[str] = []

    def acquired() -> None:
        notifications.append("acquired")
        (tmp_path / "usb-acknowledged").write_text("ready\n")

    def run() -> None:
        host.run_tool(
            [str(loader), scenario, str(tmp_path)],
            timeout=15,
            cwd=tmp_path,
            log=log,
            on_usb_ready=acquired,
        )

    if scenario == "driver-needed":
        with pytest.raises(windows_host.BootromDriverRequiredError):
            run()
    elif scenario == "missing-ready":
        with pytest.raises(SystemExit, match="without confirming its USB connection"):
            run()
    elif scenario == "transfer-error":
        with pytest.raises(SystemExit, match="failed with status 2"):
            run()
    else:
        run()

    assert notifications == (
        ["acquired"] if scenario in {"ready", "ready-crlf", "transfer-error"} else []
    )
    contents = log.read_text()
    assert contents.startswith("FPLINUX_BOOTROM_READY\nprevious invocation\n")
    assert "native discovery started\n" in contents
    if scenario in {"ready", "ready-crlf", "transfer-error"}:
        ending = b"\r\n" if scenario == "ready-crlf" else b"\n"
        assert log.read_bytes().endswith(
            b"FPLINUX_BOOTROM_READY" + ending + b"native transfer ended\n"
        )
    assert not (Path("/proc") / (tmp_path / "child.pid").read_text()).exists()


@pytest.mark.parametrize("scenario", ["timeout", "cancel"])
def test_native_usb_progress_preserves_timeout_and_cancellation_cleanup(
    tmp_path: Path, scenario: str
) -> None:
    loader = tmp_path / "loader.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/usb_loader.py", loader
    )
    acquired: list[str] = []

    def usb_ready() -> None:
        acquired.append("ready")
        if scenario == "cancel":
            raise KeyboardInterrupt

    host = StubWindowsHost({})
    error = KeyboardInterrupt if scenario == "cancel" else SystemExit
    with pytest.raises(error):
        host.run_tool(
            [str(loader), scenario, str(tmp_path)],
            timeout=3,
            cwd=tmp_path,
            log=tmp_path / "runtime.log",
            on_usb_ready=usb_ready,
        )
    assert acquired == ["ready"]
    assert (tmp_path / "usb-transfer-started").exists() is (scenario == "timeout")
    assert not (Path("/proc") / (tmp_path / "child.pid").read_text()).exists()


def test_saved_session_reappearance_cancels_native_wait_before_any_transfer(
    tmp_path: Path,
) -> None:
    loader = tmp_path / "loader.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/usb_loader.py", loader
    )
    saved = {"session_id": "previous authenticated session"}
    host = StubWindowsHost({})
    acquired: list[str] = []

    def old_session_reappeared() -> None:
        if (tmp_path / "usb-opened").exists():
            raise SessionResumedError(saved)

    with pytest.raises(SessionResumedError) as resumed:
        host.run_tool(
            [str(loader), "waiting", str(tmp_path)],
            timeout=3,
            cwd=tmp_path,
            log=tmp_path / "runtime.log",
            on_usb_ready=lambda: acquired.append("ready"),
            on_wait=old_session_reappeared,
        )

    assert resumed.value.session is saved
    assert acquired == []
    assert not (tmp_path / "usb-transfer-started").exists()
    assert not (Path("/proc") / (tmp_path / "child.pid").read_text()).exists()


def test_old_usb_probe_completing_after_native_acquisition_cannot_cancel_transfer(
    tmp_path: Path,
) -> None:
    loader = tmp_path / "loader.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/usb_loader.py", loader
    )
    host = StubWindowsHost({})
    acquired = threading.Event()
    probe_finished = threading.Event()
    saved = {"session_id": "previous authenticated session"}

    def delayed_old_usb_probe() -> bool:
        assert acquired.wait(timeout=3)
        probe_finished.set()
        return True

    with ThreadPoolExecutor(max_workers=1) as worker:
        probe = worker.submit(delayed_old_usb_probe)

        def on_wait() -> None:
            if probe.done() and probe.result():
                raise SessionResumedError(saved)
            (tmp_path / "release-ready").write_text("continue\n")

        def on_usb_ready() -> None:
            assert not (tmp_path / "usb-transfer-started").exists()
            acquired.set()
            assert probe_finished.wait(timeout=3)

        host.run_tool(
            [str(loader), "waiting", str(tmp_path)],
            timeout=3,
            cwd=tmp_path,
            log=tmp_path / "runtime.log",
            on_usb_ready=on_usb_ready,
            on_wait=on_wait,
        )
        assert probe.result(timeout=3) is True

    assert (tmp_path / "usb-transfer-started").read_text() == "acknowledged\n"
    assert not (Path("/proc") / (tmp_path / "child.pid").read_text()).exists()


def test_failed_reader_reports_stderr_and_keeps_partial_bytes(tmp_path: Path) -> None:
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    output = tmp_path / "partial.bin"
    with (
        output.open("w+b") as destination,
        pytest.raises(SystemExit, match="status 7: reader failed"),
    ):
        transport.stream_remote(session, "failure", destination, timeout=3)
    assert output.read_bytes() == b"partial"


def test_stream_timeout_reaps_the_owned_reader(tmp_path: Path) -> None:
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    pid_file = tmp_path / "reader.pid"
    with (
        (tmp_path / "partial.bin").open("w+b") as destination,
        pytest.raises(SystemExit, match="timed out"),
    ):
        transport.stream_remote(session, f"sleep:{pid_file}", destination, timeout=0.5)
    assert not (Path("/proc") / pid_file.read_text()).exists()


@pytest.mark.parametrize("identity", ["correct", "wrong-session", "wrong-kernel"])
def test_authenticated_handoff_requires_ram_and_kernel_identity(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, identity: str
) -> None:
    transport = native_process_transport(tmp_path)
    session = endpoint_session(tmp_path / "session")
    session["status"] = "prepared"
    monkeypatch.setenv(
        "FPLINUX_TEST_SESSION_ID", "ab" * 32 if identity != "wrong-session" else "ef" * 32
    )
    monkeypatch.setenv(
        "FPLINUX_TEST_KERNEL",
        "6.12.0-fplinux-1212121212121212" if identity != "wrong-kernel" else "6.12.0-other",
    )
    if identity == "correct":
        transport.authenticate(session, timeout=3)
        assert session["status"] == "ready"
        assert (
            "fplinux-0123456789abcdef0123456789abcdef ssh-ed25519 "
            in (tmp_path / "session/known_hosts").read_text()
        )
    else:
        with pytest.raises(
            SystemExit, match=r"another RAM session identity|another kernel build identity"
        ):
            transport.authenticate(session, timeout=3)
        assert session["status"] == "prepared"


@pytest.mark.parametrize("identity", ["correct", "wrong-session", "wrong-kernel"])
def test_saved_connection_reuses_the_pinned_key_and_requires_live_identities(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, identity: str
) -> None:
    """Reconnect through real SSH children while replacing native USB and peer boundaries."""
    transport = native_process_transport(tmp_path)
    saved = saved_connection(transport)
    host = transport.host
    assert isinstance(host, StubWindowsHost)
    host.snapshot["devices"] = [{"id": f"USB\\VID_0525&PID_A4A6\\{saved['usb_serial']}"}]
    known_hosts = Path(saved["known_hosts"])
    pinned = known_hosts.read_bytes()
    monkeypatch.setenv("FPLINUX_TEST_FORBID_KEYSCAN", "1")
    monkeypatch.setenv(
        "FPLINUX_TEST_SESSION_ID", "ab" * 32 if identity != "wrong-session" else "ef" * 32
    )
    monkeypatch.setenv(
        "FPLINUX_TEST_KERNEL",
        "6.12.0-fplinux-1212121212121212" if identity != "wrong-kernel" else "6.12.0-other",
    )

    current = transport.load_saved_session("test-phone", session_root=tmp_path / "data/session")
    assert current == saved
    assert current is not None
    assert transport.usb_present(current)
    if identity == "correct":
        assert (
            transport.reacquire_bound_session(current, expected_device_identity="12" * 32) == saved
        )
    else:
        with pytest.raises(
            SystemExit, match=r"another RAM session identity|another kernel build identity"
        ):
            transport.reacquire_bound_session(current, expected_device_identity="12" * 32)

    assert known_hosts.read_bytes() == pinned
    assert not Path(saved["image"]).exists()


def test_saved_ready_binding_and_keys_survive_absence_without_usb_or_ssh_discovery(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    transport = native_process_transport(tmp_path)
    saved = saved_connection(transport)
    host = transport.host
    assert isinstance(host, StubWindowsHost)
    state_path = Path(saved["directory"]) / "session.json"
    original_state = state_path.read_bytes()
    original_key = Path(saved["private_key"]).read_bytes()
    pinned = Path(saved["known_hosts"]).read_bytes()

    def unavailable_discovery(*_arguments: object, **_options: object) -> dict[str, Any]:
        pytest.fail("Loading a saved binding must not query USB or network state")

    monkeypatch.setattr(host, "ps", unavailable_discovery)
    monkeypatch.setenv("FPLINUX_TEST_FORBID_KEYSCAN", "1")

    current = transport.load_saved_session("test-phone", session_root=tmp_path / "data/session")

    assert current == saved
    assert state_path.read_bytes() == original_state
    assert Path(saved["private_key"]).read_bytes() == original_key
    assert Path(saved["known_hosts"]).read_bytes() == pinned


def test_reappearing_ready_binding_authenticates_with_a_new_network_interface_index(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    transport = native_process_transport(tmp_path)
    saved = saved_connection(transport)
    host = transport.host
    assert isinstance(host, StubWindowsHost)
    host.snapshot["devices"] = [{"id": f"USB\\VID_0525&PID_A4A6\\{saved['usb_serial']}"}]
    host.snapshot["adapters"][0]["index"] = 29
    host.snapshot["routes"][0]["index"] = 29
    state_path = Path(saved["directory"]) / "session.json"
    original_state = state_path.read_bytes()
    pinned = Path(saved["known_hosts"]).read_bytes()
    monkeypatch.setenv("FPLINUX_TEST_FORBID_KEYSCAN", "1")
    monkeypatch.setenv("FPLINUX_TEST_SESSION_ID", "ab" * 32)
    monkeypatch.setenv("FPLINUX_TEST_KERNEL", "6.12.0-fplinux-1212121212121212")

    assert transport.usb_present(saved) is True
    current = transport.reacquire_bound_session(saved, expected_device_identity="12" * 32)

    assert current is saved
    assert state_path.read_bytes() == original_state
    assert Path(saved["known_hosts"]).read_bytes() == pinned


def test_ready_connection_reopens_while_an_owned_boot_candidate_is_prepared(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An ordinary candidate waiting for USB cannot hide the retained ready connection."""
    bundle = session_preparation_bundle(tmp_path)
    bundle.runtime["usb"]["linux_gadget"]["product_id"] = 0xA4A6
    for role in ("ssh", "keyscan"):
        install_python_script(
            ROOT / "tests/fixtures/ssh_transport/windows_peer.py", bundle.tools[role]
        )
    transport = windows_host.WindowsTransport(StubWindowsHost({}), bundle)
    saved = saved_connection(transport)
    saved["target"] = "demo"
    (Path(saved["directory"]) / "session.json").write_text(json.dumps(saved), encoding="utf-8")
    host = transport.host
    assert isinstance(host, StubWindowsHost)
    candidate = windows_host._prepare_session(  # noqa: SLF001 -- exercise the ordinary producer.
        host, bundle, session_root=tmp_path / "data/session"
    )
    candidate_directory = Path(candidate["directory"])
    retained = {path.name: path.read_bytes() for path in candidate_directory.iterdir()}
    host.snapshot["devices"] = [{"id": f"USB\\VID_0525&PID_A4A6\\{saved['usb_serial']}"}]
    monkeypatch.setenv("FPLINUX_TEST_FORBID_KEYSCAN", "1")
    monkeypatch.setenv("FPLINUX_TEST_SESSION_ID", "ab" * 32)
    monkeypatch.setenv("FPLINUX_TEST_KERNEL", "6.12.0-fplinux-1212121212121212")

    current = transport.load_saved_session("demo", session_root=tmp_path / "data/session")
    assert current == saved
    assert current is not None
    assert transport.usb_present(current)
    assert transport.reacquire_bound_session(current, expected_device_identity="12" * 32) == saved

    assert candidate["status"] == "prepared"
    assert {path.name: path.read_bytes() for path in candidate_directory.iterdir()} == retained


def test_reacquiring_another_archive_identity_stops_before_ssh_authentication(
    tmp_path: Path,
) -> None:
    transport = native_process_transport(tmp_path)
    saved = saved_connection(transport)

    with pytest.raises(SystemExit, match="another kernel build identity"):
        transport.reacquire_bound_session(saved, expected_device_identity="34" * 32)


def test_concurrent_ready_authentication_preserves_saved_session_and_pinned_keys(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Two ordinary reconnects authenticate through real children without republishing state."""
    transport = native_process_transport(tmp_path)
    saved = saved_connection(transport)
    host = transport.host
    assert isinstance(host, StubWindowsHost)
    host.snapshot["devices"] = [{"id": f"USB\\VID_0525&PID_A4A6\\{saved['usb_serial']}"}]
    monkeypatch.setenv("FPLINUX_TEST_FORBID_KEYSCAN", "1")
    monkeypatch.setenv("FPLINUX_TEST_SESSION_ID", "ab" * 32)
    monkeypatch.setenv("FPLINUX_TEST_KERNEL", "6.12.0-fplinux-1212121212121212")
    state_path = Path(saved["directory"]) / "session.json"
    original_state = state_path.read_bytes()
    pinned = Path(saved["known_hosts"]).read_bytes()
    barrier = threading.Barrier(2)

    def reconnect() -> dict[str, Any]:
        barrier.wait(timeout=5)
        current = transport.load_saved_session(
            "test-phone", session_root=tmp_path / "data/session"
        )
        assert current is not None
        return transport.reacquire_bound_session(current, expected_device_identity="12" * 32)

    with state_path.open("rb") as original, ThreadPoolExecutor(max_workers=2) as workers:
        first = workers.submit(reconnect)
        second = workers.submit(reconnect)
        assert first.result(timeout=5) == saved
        assert second.result(timeout=5) == saved
        assert state_path.stat().st_ino == os.fstat(original.fileno()).st_ino

    assert state_path.read_bytes() == original_state
    assert Path(saved["known_hosts"]).read_bytes() == pinned
    assert sorted(path.name for path in state_path.parent.iterdir()) == [
        "id_ed25519",
        "known_hosts",
        "session.json",
    ]


@pytest.mark.parametrize("present", [False, True])
def test_saved_usb_presence_is_independent_of_network_readiness(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, *, present: bool
) -> None:
    """An exact USB root remains detectable while its network endpoint is unavailable."""
    transport = native_process_transport(tmp_path)
    saved = saved_connection(transport)
    host = transport.host
    assert isinstance(host, StubWindowsHost)
    host.snapshot["devices"] = (
        [{"id": f"USB\\VID_0525&PID_A4A6\\{saved['usb_serial']}"}] if present else []
    )
    host.snapshot["adapters"][0]["status"] = "Down"
    pinned = Path(saved["known_hosts"]).read_bytes()
    now = 0.0

    def advance_clock(delay: float) -> None:
        nonlocal now
        now += delay

    monkeypatch.setattr("fplinux_cli.runtime.windows_host.time.monotonic", lambda: now)
    monkeypatch.setattr("fplinux_cli.runtime.windows_host.time.sleep", advance_clock)

    current = transport.load_saved_session("test-phone", session_root=tmp_path / "data/session")
    assert current == saved
    assert current is not None
    assert transport.usb_present(current) is present
    assert now == 0
    if present:
        with pytest.raises(SystemExit, match="did not become ready before the deadline"):
            transport.reacquire_bound_session(current, expected_device_identity="12" * 32)
        assert now >= 3
    assert Path(saved["known_hosts"]).read_bytes() == pinned


def test_ambiguous_saved_phone_connections_stop_before_authentication(
    tmp_path: Path,
) -> None:
    """Two valid saved identities cannot select one retained RAM session."""
    transport = native_process_transport(tmp_path)
    saved = saved_connection(transport)
    second_id = "ef" * 32
    serial = hashlib.sha256(bytes.fromhex(second_id)).hexdigest()[:32]
    directory = tmp_path / "data/session" / serial
    directory.mkdir()
    for name in ("id_ed25519", "known_hosts"):
        (directory / name).write_text("another fixture private key\n")
    second = {
        **saved,
        "session_id": second_id,
        "usb_serial": serial,
        "directory": str(directory),
        "private_key": str(directory / "id_ed25519"),
        "known_hosts": str(directory / "known_hosts"),
        "image": str(directory / "image.bin"),
    }
    (directory / "session.json").write_text(json.dumps(second))
    with pytest.raises(SystemExit, match="Multiple saved phone connections cannot select"):
        transport.load_saved_session("test-phone", session_root=tmp_path / "data/session")


def test_session_network_avoids_host_routes_and_saved_sessions(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Real session publication and personalization preserve collision avoidance."""
    archive = tmp_path / "archive"
    archive.mkdir()
    bundle = session_preparation_bundle(archive)
    original_image = (archive / bundle.runtime["image"]).read_bytes()
    local_data = tmp_path / "local-data"
    previous = archive / "data/session/previous"
    previous.mkdir(parents=True)
    (previous / "session.json").write_text(json.dumps({"network": "172.16.0.0/30"}))
    old_session = local_data / "FPLinux/demo/previous/session.json"
    old_session.parent.mkdir(parents=True)
    old_session.write_text("unrelated old local data\n")
    monkeypatch.setenv("LOCALAPPDATA", str(local_data))
    random_material = iter(
        [
            bytes.fromhex("0000000001"),
            bytes.fromhex("0100000000"),
            bytes.fromhex("0200000000"),
            b"i" * 32,
            b"h" * 6,
            b"d" * 6,
            b"s" * 64,
        ]
    )

    def random_bytes(size: int) -> bytes:
        material = next(random_material)
        assert len(material) == size
        return material

    monkeypatch.setattr(secrets, "token_bytes", random_bytes)
    host = StubWindowsHost(
        {
            "routes": [
                {"index": 3, "prefix": "10.0.0.0/8", "next_hop": "192.0.2.1"},
                {"index": 3, "prefix": "0.0.0.0/0", "next_hop": "192.0.2.1"},
            ]
        }
    )

    events: list[str] = []
    session = windows_host._prepare_session(  # noqa: SLF001 -- real session consumer.
        host, bundle, session_root=archive / "data/session", events=events.append
    )

    assert events == ["network-preparation", "session-preparation", "payload-preparation"]
    assert session["network"] == "192.168.0.0/30"
    assert session["host_address"] == "192.168.0.1"
    assert session["phone_address"] == "192.168.0.2"
    assert session["status"] == "prepared"
    directory = Path(session["directory"])
    assert directory.parent == archive / "data/session"
    assert old_session.read_text() == "unrelated old local data\n"
    assert json.loads((directory / "session.json").read_text()) == session
    personalized = Path(session["image"]).read_bytes()
    assert (archive / bundle.runtime["image"]).read_bytes() == original_image
    assert personalized[:1024] == original_image[:1024]
    assert personalized[1536:] == original_image[1536:]
    assert personalized[1040:1072] == b"i" * 32
    assert b"host_address=192.168.0.1\n" in personalized[1204:1460]
    assert b"phone_address=192.168.0.2\n" in personalized[1204:1460]


def test_failed_key_generation_discards_only_the_new_session(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A real key-generator stand-in leaves partial keys before returning an error."""
    archive = tmp_path / "archive"
    archive.mkdir()
    bundle = session_preparation_bundle(archive)
    previous = archive / "data/session/previous/session.json"
    previous.parent.mkdir(parents=True)
    previous.write_text('{"network": "172.16.0.0/30"}\n')
    monkeypatch.setenv("FPLINUX_TEST_KEYGEN_STATUS", "7")
    host = StubWindowsHost({"routes": []})

    with pytest.raises(SystemExit, match="SSH key generation failed"):
        windows_host._prepare_session(  # noqa: SLF001 -- real session consumer.
            host, bundle, session_root=archive / "data/session"
        )

    assert previous.read_text() == '{"network": "172.16.0.0/30"}\n'
    assert list((archive / "data/session").iterdir()) == [previous.parent]


def test_resuming_saved_session_discards_only_the_unstarted_windows_boot_candidate(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    archive = tmp_path / "archive"
    archive.mkdir()
    bundle = session_preparation_bundle(archive)
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_loader_output.py",
        bundle.tools["loader"],
    )
    transport = windows_host.WindowsTransport(StubWindowsHost({}), bundle)
    saved = saved_connection(transport)
    saved["target"] = "demo"
    state_path = Path(saved["directory"]) / "session.json"
    state_path.write_text(json.dumps(saved), encoding="utf-8")
    retained = {path.name: path.read_bytes() for path in state_path.parent.iterdir()}
    host = transport.host
    assert isinstance(host, StubWindowsHost)
    host.snapshot["devices"] = []
    monkeypatch.setattr(windows_host, "WindowsHost", lambda: host)
    pid_file = tmp_path / "loader.pid"
    monkeypatch.setenv("FPLINUX_TEST_PENDING_LOADER_PID", str(pid_file))
    capsule = tmp_path / "device-data.cpio"
    capsule.write_bytes(b"controlled phone data\n")
    events: list[str] = []

    def resumed() -> None:
        if pid_file.exists():
            raise SessionResumedError(saved)

    with pytest.raises(SessionResumedError) as result:
        windows_host.boot_archive(
            bundle,
            capsule,
            session_root=archive / "data/session",
            events=events.append,
            on_wait=resumed,
        )

    assert result.value.session is saved
    assert "ram-loader-start" not in events
    assert list((archive / "data/session").iterdir()) == [state_path.parent]
    assert {path.name: path.read_bytes() for path in state_path.parent.iterdir()} == retained
    assert not (Path("/proc") / pid_file.read_text()).exists()


@pytest.mark.parametrize(
    ("loader_status", "bridge_status", "capsule_layout"),
    [
        (0, 0, "bundle"),
        (0, 0, "external"),
        (7, 0, "external"),
        (0, 7, "external"),
    ],
    ids=["bundle", "external", "loader-failure", "bridge-failure"],
)
# The case dimensions and owned process resources remain explicit at the test boundary.
def test_windows_loader_processes_receive_private_capsule_and_retain_one_archive_log(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    loader_status: int,
    bridge_status: int,
    capsule_layout: str,
) -> None:
    """Use native-process stand-ins; PnP and SSH remain controlled Windows boundaries."""
    archive = tmp_path / "archive"
    archive.mkdir()
    root = archive_files(archive)
    manifest_path = root / "windows-host.json"
    manifest = json.loads(manifest_path.read_text())
    runtime_path = root / "runtime-manifest.json"
    runtime = json.loads(runtime_path.read_text())
    for relative in ("tools/spd_dump.exe", "tools/libc_server.exe"):
        destination = root / relative
        install_python_script(
            ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_loader_output.py",
            destination,
        )
        runtime["sha256"][relative] = hashlib.sha256(destination.read_bytes()).hexdigest()
    runtime_path.write_text(json.dumps(runtime))
    manifest_path.write_text(json.dumps(manifest))
    build_manifest_path = root / "build-manifest.json"
    build_manifest = json.loads(build_manifest_path.read_text())
    runtime_bytes = (root / "runtime-manifest.json").read_bytes()
    build_manifest["files"]["runtime-manifest.json"]["sha256"] = hashlib.sha256(
        runtime_bytes
    ).hexdigest()
    build_manifest["files"]["runtime-manifest.json"]["size"] = len(runtime_bytes)
    del build_manifest["generation"]
    canonical = (json.dumps(build_manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()
    build_manifest["generation"] = hashlib.sha256(canonical).hexdigest()
    build_manifest_path.write_text(json.dumps(build_manifest))
    log = root / "logs/20261011T123456Z.log"
    log.parent.mkdir()
    log.write_text("earlier run diagnostics\n")
    session = endpoint_session(root / "data/session/prepared")
    image = Path(session["directory"]) / "image.bin"
    image.write_bytes(b"prepared image")
    session["image"] = str(image)
    capsule_parent = root if capsule_layout == "bundle" else root / "data/phone"
    capsule_parent.mkdir(parents=True, exist_ok=True)
    capsule = capsule_parent / "device-data.cpio"
    capsule.write_bytes(b"controlled capsule")
    expected_payload = {"device-data.cpio": b"controlled capsule"}
    events: list[str] = []
    monkeypatch.setenv("FPLINUX_TEST_LOADER_STATUS", str(loader_status))
    monkeypatch.setenv("FPLINUX_TEST_BRIDGE_STATUS", str(bridge_status))
    bridge_payload = tmp_path / "bridge-payload.json"
    monkeypatch.setenv("FPLINUX_TEST_BRIDGE_PAYLOAD", str(bridge_payload))
    protected_payload: dict[str, bytes] = {}

    class SelectedWindowsHost(StubWindowsHost):
        """Return the selected WinUSB record once; later PnP responses omit it."""

        def __init__(self) -> None:
            super().__init__({})
            self.selected = False

        def secure_new(self, path: Path, *, directory: bool = False) -> None:
            super().secure_new(path, directory=directory)
            if not directory and path.parent.name == "payload":
                protected_payload[path.name] = path.read_bytes()

        @override
        def run_tool(
            self,
            argv: list[str],
            *,
            timeout: int,
            cwd: Path,
            log: Path,
            environment: dict[str, str] | None = None,
            on_usb_ready: Callable[[], None] | None = None,
            on_wait: Callable[[], None] | None = None,
        ) -> None:
            """Retain native discovery time without consuming the upload's existing budget."""
            if Path(argv[0]).name == "spd_dump.exe":
                assert argv[argv.index("--wait") + 1] == "30"
                assert timeout == 330
            super().run_tool(
                argv,
                timeout=timeout,
                cwd=cwd,
                log=log,
                environment=environment,
                on_usb_ready=on_usb_ready,
                on_wait=on_wait,
            )

        def ps(
            self,
            script: str,
            *arguments: str,
            timeout: float = 20,
            cancelled: Callable[[], bool] | None = None,
        ) -> dict[str, Any]:
            del script, arguments, timeout, cancelled
            assert "waiting-for-device" not in events or "ram-loader-start" in events
            if "ram-loader-start" in events and not self.selected:
                self.selected = True
                return {
                    "devices": [{"id": "USB\\VID_1782&PID_4D00\\selected", "service": "WinUSB"}]
                }
            return {"devices": []}

    host = SelectedWindowsHost()
    monkeypatch.setattr(windows_host, "WindowsHost", lambda: host)

    def prepared_session(
        _host: windows_host.WindowsHost,
        _bundle: windows_host.WindowsBundle,
        *,
        events: object = None,
        session_root: Path,
        prepared_data_sha256: str | None = None,
    ) -> dict[str, Any]:
        del events, session_root, prepared_data_sha256
        return session

    def authenticated(
        _transport: windows_host.WindowsTransport, selected: dict[str, Any], *, timeout: int
    ) -> None:
        del timeout
        selected["status"] = "ready"

    monkeypatch.setattr(windows_host, "_prepare_session", prepared_session)
    monkeypatch.setattr(windows_host.WindowsTransport, "authenticate", authenticated)

    if loader_status or bridge_status:
        with pytest.raises(SystemExit, match="failed with status 7"):
            windows_host.boot_archive(
                admitted_windows(root),
                capsule,
                session_root=root / "data/session",
                events=events.append,
                log=log,
            )
        invocations = 1 if loader_status else 2
        assert log.read_text() == (
            "earlier run diagnostics\nFPLINUX_BOOTROM_READY\n"
            + invocations * "controlled loader output\ncontrolled loader diagnostic\n"
        )
        assert not Path(session["directory"]).exists()
        assert capsule.read_bytes() == b"controlled capsule"
        if bridge_status:
            observed_maps = json.loads(bridge_payload.read_text())
            assert observed_maps == {
                name: contents.hex() for name, contents in expected_payload.items()
            }
            assert protected_payload == expected_payload
        else:
            assert not bridge_payload.exists()
        return

    _transport, ready = windows_host.boot_archive(
        admitted_windows(root),
        capsule,
        session_root=root / "data/session",
        events=events.append,
        log=log,
    )

    assert ready["status"] == "ready"
    observed_maps = json.loads(bridge_payload.read_text())
    assert observed_maps == {name: contents.hex() for name, contents in expected_payload.items()}
    assert protected_payload == expected_payload
    assert log.read_text() == (
        "earlier run diagnostics\n"
        "FPLINUX_BOOTROM_READY\n"
        "controlled loader output\ncontrolled loader diagnostic\n"
        "controlled loader output\ncontrolled loader diagnostic\n"
    )
    assert not list(Path(session["directory"]).rglob("*.log"))
    assert not image.exists()
    assert not (Path(session["directory"]) / "fdl1.bin").exists()
    assert not (Path(session["directory"]) / "payload").exists()
    assert Path(session["private_key"]).exists()


def test_two_boots_retain_one_ready_session_and_failed_next_boot_preserves_its_keys(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Execute real session, loader and SSH processes with controlled native USB/ACL state."""
    archive = tmp_path / "archive"
    archive.mkdir()
    bundle = session_preparation_bundle(archive)
    for role, source in {
        "loader": "tests/host_process/runtime/fixtures/windows-host/windows_loader_output.py",
        "bridge": "tests/host_process/runtime/fixtures/windows-host/windows_loader_output.py",
        "ssh": "tests/fixtures/ssh_transport/windows_peer.py",
        "keyscan": "tests/fixtures/ssh_transport/windows_peer.py",
    }.items():
        install_python_script(ROOT / source, bundle.tools[role])
    runtime = bundle.runtime
    for relative in runtime["sha256"]:
        runtime["sha256"][relative] = hashlib.sha256((archive / relative).read_bytes()).hexdigest()
    runtime_path = archive / "runtime-manifest.json"
    runtime_path.write_text(json.dumps(runtime))
    windows_path = archive / "windows-host.json"
    windows = json.loads(windows_path.read_text())
    for relative in windows["sha256"]:
        windows["sha256"][relative] = hashlib.sha256((archive / relative).read_bytes()).hexdigest()
    windows_path.write_text(json.dumps(windows))
    build_path = archive / "build-manifest.json"
    build = json.loads(build_path.read_text())
    for relative, record in build["files"].items():
        contents = (archive / relative).read_bytes()
        record["sha256"] = hashlib.sha256(contents).hexdigest()
        record["size"] = len(contents)
    del build["generation"]
    canonical = (json.dumps(build, sort_keys=True, separators=(",", ":")) + "\n").encode()
    build["generation"] = hashlib.sha256(canonical).hexdigest()
    build_path.write_text(json.dumps(build))
    active = tmp_path / "active-session-path"
    monkeypatch.setenv("FPLINUX_TEST_ACTIVE_SESSION", str(active))
    monkeypatch.setenv("FPLINUX_TEST_KERNEL", "6.12.0-fplinux-1212121212121212")

    class ArchiveWindowsHost(StubWindowsHost):
        """Expose the handset after invitation, then its selected generated DHCP session."""

        def __init__(self) -> None:
            super().__init__({})
            self.bootrom = False

        def ps(
            self,
            script: str,
            *arguments: str,
            timeout: float = 20,
            cancelled: Callable[[], bool] | None = None,
        ) -> dict[str, Any]:
            del arguments, timeout, cancelled
            if script == "routes.ps1":
                return {"routes": []}
            if script == "devices.ps1":
                devices = (
                    [{"id": "USB\\VID_1782&PID_4D00\\selected", "service": "WinUSB"}]
                    if self.bootrom
                    else []
                )
                return {"devices": devices}
            assert script == "network.ps1"
            session = json.loads(Path(active.read_text()).read_text())
            snapshot = endpoint_snapshot()
            adapter = snapshot["adapters"][0]
            adapter["mac"] = session["host_mac"]
            adapter["lineage"][-1] = (
                f"USB\\VID_{session['vendor_id']:04X}&PID_{session['product_id']:04X}\\"
                f"{session['usb_serial']}"
            )
            adapter["addresses"][0]["address"] = session["host_address"]
            snapshot["routes"][0]["prefix"] = session["network"]
            return snapshot

        def progress(self, stage: str) -> None:
            if stage == "waiting-for-device":
                self.bootrom = True
            elif stage == "ram-loader-complete":
                self.bootrom = False

    host = ArchiveWindowsHost()
    monkeypatch.setattr(windows_host, "WindowsHost", lambda: host)
    capsule = archive / "data/phone/device-data.cpio"
    capsule.parent.mkdir(parents=True)
    capsule.write_bytes(b"controlled capsule")
    unrelated = archive / "data/session/notes/keep.txt"
    unrelated.parent.mkdir(parents=True)
    unrelated.write_text("unrelated user data\n")

    _transport, first = windows_host.boot_archive(
        admitted_windows(archive),
        capsule,
        session_root=archive / "data/session",
        events=host.progress,
        log=archive / "logs/first.log",
    )
    first_directory = Path(first["directory"])
    _transport, second = windows_host.boot_archive(
        admitted_windows(archive),
        capsule,
        session_root=archive / "data/session",
        events=host.progress,
        log=archive / "logs/second.log",
    )

    assert first["usb_serial"] != second["usb_serial"]
    assert not first_directory.exists()
    second_directory = Path(second["directory"])
    assert list((archive / "data/session").glob("*/session.json")) == [
        second_directory / "session.json"
    ]
    assert not (second_directory / "image.bin").exists()
    assert not (second_directory / "payload").exists()
    private_key = Path(second["private_key"]).read_bytes()
    known_hosts = Path(second["known_hosts"]).read_bytes()
    monkeypatch.setenv("FPLINUX_TEST_LOADER_STATUS", "7")

    with pytest.raises(SystemExit, match="failed with status 7"):
        windows_host.boot_archive(
            admitted_windows(archive),
            capsule,
            session_root=archive / "data/session",
            events=host.progress,
            log=archive / "logs/failed.log",
        )

    assert Path(second["private_key"]).read_bytes() == private_key
    assert Path(second["known_hosts"]).read_bytes() == known_hosts
    assert list((archive / "data/session").glob("*/session.json")) == [
        second_directory / "session.json"
    ]
    assert unrelated.read_text() == "unrelated user data\n"
    assert capsule.read_bytes() == b"controlled capsule"


@dataclass(frozen=True)
class Sc9820eWindowsBoot:
    """Own one admitted RAM archive and its explicitly controlled Windows boundaries."""

    bundle: windows_host.WindowsBundle
    capsule: Path
    trace: Path
    canonical: dict[str, bytes]
    events: list[str]
    progress: Callable[[str], None]


@pytest.fixture
def sc9820e_windows_boot(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Sc9820eWindowsBoot:
    """Run session, upload and SSH processes; substitute unavailable PnP and ACL APIs."""
    archive = tmp_path / "Windows архив with spaces"
    archive.mkdir()
    archive_files(archive, platform="sc9820e")
    windows_path = archive / "windows-host.json"
    windows = json.loads(windows_path.read_text())
    for role, source in {
        "loader": "tests/host_process/runtime/fixtures/windows-host/sc9820e_loader.py",
        "keygen": "tests/fixtures/runtime-tools/windows_keygen.py",
        "ssh": "tests/fixtures/ssh_transport/windows_peer.py",
        "keyscan": "tests/fixtures/ssh_transport/windows_peer.py",
    }.items():
        install_python_script(ROOT / source, archive / windows["host_tools"][role])
    for relative in windows["sha256"]:
        windows["sha256"][relative] = hashlib.sha256((archive / relative).read_bytes()).hexdigest()
    windows_path.write_text(json.dumps(windows))
    refresh_archive(archive)
    bundle = admitted_windows(archive)
    active = tmp_path / "active-session-path"
    trace = tmp_path / "sc9820e-uploads.json"
    monkeypatch.setenv("FPLINUX_TEST_ACTIVE_SESSION", str(active))
    monkeypatch.setenv("FPLINUX_TEST_SC9820E_UPLOADS", str(trace))
    monkeypatch.setenv("FPLINUX_TEST_KERNEL", "6.12.0-fplinux-1212121212121212")
    events: list[str] = []

    class ArchiveWindowsHost(StubWindowsHost):
        """Supply the generated session's USB ancestry, DHCP lease and selected device."""

        def __init__(self) -> None:
            super().__init__({})
            self.bootrom = False

        def ps(
            self,
            script: str,
            *arguments: str,
            timeout: float = 20,
            cancelled: Callable[[], bool] | None = None,
        ) -> dict[str, Any]:
            del arguments, timeout, cancelled
            if script == "routes.ps1":
                return {"routes": []}
            if script == "devices.ps1":
                devices = (
                    [{"id": "USB\\VID_1782&PID_4D00\\selected", "service": "WinUSB"}]
                    if self.bootrom
                    else []
                )
                return {"devices": devices}
            assert script == "network.ps1"
            session = json.loads(Path(active.read_text()).read_text())
            snapshot = endpoint_snapshot()
            adapter = snapshot["adapters"][0]
            adapter["mac"] = session["host_mac"]
            adapter["lineage"][-1] = (
                f"USB\\VID_{session['vendor_id']:04X}&PID_{session['product_id']:04X}\\"
                f"{session['usb_serial']}"
            )
            adapter["addresses"][0]["address"] = session["host_address"]
            snapshot["routes"][0]["prefix"] = session["network"]
            return snapshot

        def progress(self, stage: str) -> None:
            events.append(stage)
            if stage == "waiting-for-device":
                self.bootrom = True
            elif stage == "ram-loader-complete":
                self.bootrom = False

    host = ArchiveWindowsHost()
    monkeypatch.setattr(windows_host, "WindowsHost", lambda: host)
    capsule = archive / "data/phone/device-data.cpio"
    capsule.parent.mkdir(parents=True)
    capsule.write_bytes(create_device_data_initramfs({}, usb_transport="rndis"))
    canonical = {
        relative: (archive / relative).read_bytes()
        for relative in (bundle.runtime["image"], *bundle.runtime["assets"].values())
    }
    return Sc9820eWindowsBoot(bundle, capsule, trace, canonical, events, host.progress)


def test_sc9820e_windows_boot_uploads_the_ordered_ram_chain_and_authenticates(
    sc9820e_windows_boot: Sc9820eWindowsBoot,
) -> None:
    """Exercise real child handshakes and bytes without claiming native PE or USB coverage."""
    case = sc9820e_windows_boot
    root = case.bundle.root
    capsule = case.capsule.read_bytes()

    _transport, session = windows_host.boot_archive(
        case.bundle,
        case.capsule,
        session_root=root / "data/session",
        events=case.progress,
        log=root / "logs/run.log",
        prepared_data_sha256=hashlib.sha256(capsule).hexdigest(),
    )

    observed = json.loads(case.trace.read_text())
    uploads = {name: bytes.fromhex(contents) for name, contents in observed["files"].items()}
    initramfs = b"root123\0" + capsule
    image = bytes.fromhex(observed["image"])
    slot = case.bundle.runtime["personalization"]["offset"]
    record = image[slot : slot + 512]
    expected_tree = binary_tree(
        [("decoy", b"\xa1" * 64 + b"\xc3" * 32 + b"\xb2" * 68 + b"\xd4" * 256)],
        [
            (
                "chosen",
                [
                    ("rng-seed", record[48:112]),
                    ("linux,initrd-start", bytes.fromhex("85500000")),
                    ("linux,initrd-end", struct.pack(">I", 0x85500000 + len(initramfs))),
                ],
                [],
            ),
            (
                "fplinux-session",
                [
                    ("compatible", b"fplinux,ram-session\0"),
                    ("fplinux,session-id", bytes.fromhex(session["session_id"])),
                    ("fplinux,ssh-client-key", record[112:180]),
                    ("fplinux,usb-session", record[180:436]),
                ],
                [],
            ),
        ],
    )
    assert uploads == {
        "fdl1.bin": b"DHTBfirst-stage",
        "sml.bin": b"secure-monitor",
        "fdl2.bin": b"DHTBboard-initializer",
        "entry.bin": b"entry123",
        "kernel.bin": b"kernel123",
        "dtb.bin": expected_tree,
        "initramfs.bin": initramfs,
    }
    assert capsule_files(uploads["initramfs.bin"][8:]) == {"etc/fplinux/usb-transport": b"rndis\n"}
    assert observed["argv"] == [
        "--ready-ack",
        "--verbose",
        "1",
        "--wait",
        "30",
        "keep_charge",
        "1",
        "fdl",
        "fdl1.bin",
        "0x5000",
        "write_data",
        "0x8e000000",
        "0",
        "14",
        "sml.bin",
        "write_data",
        "0x81008000",
        "0",
        "9",
        "kernel.bin",
        "write_data",
        "0x85400000",
        "0",
        str(len(expected_tree)),
        "dtb.bin",
        "write_data",
        "0x85500000",
        "0",
        str(len(initramfs)),
        "initramfs.bin",
        "write_data",
        "0x80002060",
        "0",
        "8",
        "entry.bin",
        "timeout",
        "60000",
        "fdl_exec",
        "fdl2.bin",
        "0x9efffe00",
    ]
    directory = Path(session["directory"])
    assert Path(observed["cwd"]) == directory / "uploads"
    assert session["status"] == "ready"
    assert session["prepared_data_sha256"] == hashlib.sha256(capsule).hexdigest()
    assert Path(session["private_key"]).read_bytes() == b"fixture private material\n"
    assert Path(session["known_hosts"]).read_text().startswith(f"fplinux-{session['usb_serial']} ")
    assert not (directory / "image.bin").exists()
    assert not (directory / "uploads").exists()
    assert not (directory / "payload").exists()
    assert case.events[-3:] == ["linux-start", "linux-transition", "ssh-ready"]
    assert (root / "logs/run.log").read_text() == (
        "FPLINUX_BOOTROM_READY\nSC9820E RAM uploads acknowledged\n"
    )
    assert case.capsule.read_bytes() == capsule
    for relative, contents in case.canonical.items():
        assert (root / relative).read_bytes() == contents


@pytest.mark.parametrize("failure", ["loader", "authentication", "cancellation"])
def test_sc9820e_windows_boot_failure_reaps_the_loader_and_removes_session_material(
    sc9820e_windows_boot: Sc9820eWindowsBoot,
    monkeypatch: pytest.MonkeyPatch,
    failure: str,
) -> None:
    """A failed real loader, authenticated identity or owner cancellation leaves no keys."""
    case = sc9820e_windows_boot
    root = case.bundle.root
    capsule = case.capsule.read_bytes()
    error: type[BaseException] = SystemExit
    message: str | None = None
    if failure == "loader":
        monkeypatch.setenv("FPLINUX_TEST_LOADER_STATUS", "7")
        message = "spd_dump.exe failed with status 7"
    elif failure == "authentication":
        monkeypatch.setenv("FPLINUX_TEST_KERNEL", "6.12.0-fplinux-ffffffffffffffff")
        message = "another kernel build identity"
    else:
        error = KeyboardInterrupt

    def progress(stage: str) -> None:
        case.progress(stage)
        if failure == "cancellation" and stage == "ram-loader-start":
            raise KeyboardInterrupt

    with pytest.raises(error, match=message):
        windows_host.boot_archive(
            case.bundle,
            case.capsule,
            session_root=root / "data/session",
            events=progress,
            log=root / "logs/failed.log",
        )

    observed = json.loads(case.trace.read_text())
    assert not (Path("/proc") / str(observed["pid"])).exists()
    assert not Path(observed["cwd"]).parent.exists()
    assert not list((root / "data/session").glob("*/session.json"))
    assert case.capsule.read_bytes() == capsule
    for relative, contents in case.canonical.items():
        assert (root / relative).read_bytes() == contents


def test_powershell_discovery_timeout_reaps_the_owned_process(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Production discovery timeout cleans up a real native-command stand-in."""
    pid_file = tmp_path / "powershell.pid"
    monkeypatch.setenv("FPLINUX_TEST_DISCOVERY_PID", str(pid_file))
    powershell = tmp_path / "powershell.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_discovery_timeout.py",
        powershell,
    )
    # Construction needs Windows; only the production subprocess/JSON boundary is exercised.
    host = windows_host.WindowsHost.__new__(windows_host.WindowsHost)
    host.powershell = powershell

    with pytest.raises(subprocess.TimeoutExpired):
        host.ps("routes.ps1", timeout=1)

    assert not (Path("/proc") / pid_file.read_text()).exists()


@pytest.mark.parametrize(
    ("response", "expected", "error"),
    [
        (
            '\ufeff{"owner":"Юникод", "protected":true}',
            {"owner": "Юникод", "protected": True},
            None,
        ),
        ('["not an object"]', None, "Windows discovery returned an unexpected result"),
    ],
)
def test_powershell_file_arguments_remain_literal_and_json_result_is_checked(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    response: str,
    expected: dict[str, object] | None,
    error: str | None,
) -> None:
    """Exercise argv and UTF-8 transport with a real child replacing native PowerShell."""
    arguments_file = tmp_path / "arguments.json"
    monkeypatch.setenv("FPLINUX_TEST_POWERSHELL_ARGUMENTS", str(arguments_file))
    monkeypatch.setenv("FPLINUX_TEST_POWERSHELL_RESULT", response)
    powershell = tmp_path / "powershell.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_powershell_boundary.py",
        powershell,
    )
    host = windows_host.WindowsHost.__new__(windows_host.WindowsHost)
    host.powershell = powershell
    private_path = tmp_path / "Private ' USB $(no-command) Юникод/session.json"

    if error is None:
        assert host.ps("acl.ps1", "-Path", str(private_path), timeout=3) == expected
    else:
        with pytest.raises(SystemExit, match=error):
            host.ps("acl.ps1", "-Path", str(private_path), timeout=3)

    assert json.loads(arguments_file.read_text(encoding="utf-8")) == [
        "-NoLogo",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(ROOT / "common/windows/acl.ps1"),
        "-Path",
        str(private_path),
    ]
    assert (ROOT / "common/windows/acl.ps1").is_file()


def test_network_discovery_receives_the_exact_usb_session_identity(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The carrier process records arguments; it does not execute native PnP queries."""
    arguments_file = tmp_path / "network-arguments.json"
    monkeypatch.setenv("FPLINUX_TEST_POWERSHELL_ARGUMENTS", str(arguments_file))
    monkeypatch.setenv("FPLINUX_TEST_POWERSHELL_RESULT", '{"adapters": [], "routes": []}')
    powershell = tmp_path / "powershell.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_powershell_boundary.py",
        powershell,
    )
    host = windows_host.WindowsHost.__new__(windows_host.WindowsHost)
    host.powershell = powershell
    session = endpoint_session(tmp_path / "session")

    assert host.network_snapshot(session, timeout=3) == {"adapters": [], "routes": []}

    assert json.loads(arguments_file.read_text())[-9:] == [
        str(ROOT / "common/windows/network.ps1"),
        "-HostMac",
        "02:00:00:00:00:01",
        "-UsbSerial",
        "0123456789abcdef0123456789abcdef",
        "-VendorId",
        "1317",
        "-ProductId",
        "42150",
    ]
