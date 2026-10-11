# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real SSH child lifetime and bytes with a replaced Windows discovery boundary."""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest

from common import windows_host
from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.windows_host_support import (
    StubWindowsHost,
    archive_files,
    endpoint_session,
    endpoint_snapshot,
    native_process_transport,
    saved_connection,
    session_preparation_bundle,
)


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
def explicit_public_key_acl(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> WindowsAclBoundary:
    """Model ssh-keygen's explicit Everyone grant using real boundary processes."""
    path = tmp_path / "client-key.pub"
    path.write_text("controlled public key\n", encoding="ascii")
    state = tmp_path / "acl.json"
    sid = "S-1-5-21-100-200-300-1001"
    state.write_text(
        json.dumps(
            {
                "owner": sid,
                "protected": True,
                "rules": [
                    {"sid": trustee, "type": "Allow", "inherited": False}
                    for trustee in ("S-1-1-0", sid, "S-1-5-18", "S-1-5-32-544")
                ],
            }
        ),
        encoding="utf-8",
    )
    monkeypatch.setenv("FPLINUX_TEST_ACL_STATE", str(state))
    icacls = tmp_path / "icacls.exe"
    powershell = tmp_path / "powershell.exe"
    install_python_script(ROOT / "tests/fixtures/processes/windows_icacls_boundary.py", icacls)
    install_python_script(ROOT / "tests/fixtures/processes/windows_acl_boundary.py", powershell)
    host = windows_host.WindowsHost.__new__(windows_host.WindowsHost)
    host.sid = sid
    host.icacls = icacls
    host.powershell = powershell
    return WindowsAclBoundary(host, path, state)


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

    assert transport.open_shell(session) == status

    captured = capfd.readouterr()
    assert captured.out == "controlled shell output\n"
    assert captured.err == "controlled shell diagnostic\n"


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
            transport.open_shell(session)
    finally:
        cancelled.set()
        trigger.join()

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
    install_python_script(ROOT / "tests/fixtures/processes/windows_loader_output.py", loader)
    host = StubWindowsHost({})
    log = tmp_path / "loader.log"

    host.run_tool([str(loader)], timeout=3, cwd=tmp_path, log=log)

    assert log.read_text() == "controlled loader output\ncontrolled loader diagnostic\n"


def test_loader_debug_environment_is_local_to_the_owned_child(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    loader = tmp_path / "loader.exe"
    install_python_script(ROOT / "tests/fixtures/processes/windows_loader_output.py", loader)
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

    if identity == "correct":
        current = transport.load_current_session("test-phone")
        assert current == saved
    else:
        with pytest.raises(
            SystemExit, match=r"another RAM session identity|another kernel build identity"
        ):
            transport.load_current_session("test-phone")

    assert known_hosts.read_bytes() == pinned
    assert not Path(saved["image"]).exists()


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
        host, bundle, events=events.append
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
        windows_host._prepare_session(host, bundle)  # noqa: SLF001 -- real session consumer.

    assert previous.read_text() == '{"network": "172.16.0.0/30"}\n'
    assert list((archive / "data/session").iterdir()) == [previous.parent]


@pytest.mark.parametrize("loader_status", [0, 7])
def test_selected_winusb_snapshot_reaches_native_loader_and_one_archive_log(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, loader_status: int
) -> None:
    """Use native-process stand-ins; PnP and SSH remain controlled Windows boundaries."""
    archive = tmp_path / "archive"
    archive.mkdir()
    root = archive_files(archive)
    manifest_path = root / "windows-host.json"
    manifest = json.loads(manifest_path.read_text())
    for relative in ("tools/spd_dump.exe", "tools/libc_server.exe"):
        destination = root / relative
        install_python_script(
            ROOT / "tests/fixtures/processes/windows_loader_output.py", destination
        )
        manifest["sha256"][relative] = hashlib.sha256(destination.read_bytes()).hexdigest()
    manifest_path.write_text(json.dumps(manifest))
    log = root / "logs/20261011T123456Z.log"
    log.parent.mkdir()
    log.write_text("earlier run diagnostics\n")
    session = endpoint_session(root / "data/session/prepared")
    image = Path(session["directory"]) / "image.bin"
    image.write_bytes(b"prepared image")
    session["image"] = str(image)
    capsule = root / "data/phone/device-data.cpio"
    capsule.parent.mkdir(parents=True)
    capsule.write_bytes(b"controlled capsule")
    events: list[str] = []
    monkeypatch.setenv("FPLINUX_TEST_LOADER_STATUS", str(loader_status))

    class SelectedWindowsHost(StubWindowsHost):
        """Return the selected WinUSB record once; later PnP responses omit it."""

        def __init__(self) -> None:
            super().__init__({})
            self.selected = False

        def ps(self, script: str, *arguments: str, timeout: float = 20) -> dict[str, Any]:
            del script, arguments, timeout
            if "waiting-for-device" in events and not self.selected:
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
    ) -> dict[str, Any]:
        del events
        return session

    def authenticated(
        _transport: windows_host.WindowsTransport, selected: dict[str, Any], *, timeout: int
    ) -> None:
        del timeout
        selected["status"] = "ready"

    monkeypatch.setattr(windows_host, "_prepare_session", prepared_session)
    monkeypatch.setattr(windows_host.WindowsTransport, "authenticate", authenticated)

    if loader_status:
        with pytest.raises(SystemExit, match="failed with status 7"):
            windows_host.boot_archive(root, capsule, events=events.append, log=log)
        assert log.read_text() == (
            "earlier run diagnostics\ncontrolled loader output\ncontrolled loader diagnostic\n"
        )
        assert not Path(session["directory"]).exists()
        assert capsule.read_bytes() == b"controlled capsule"
        return

    _transport, ready = windows_host.boot_archive(root, capsule, events=events.append, log=log)

    assert ready["status"] == "ready"
    assert log.read_text() == (
        "earlier run diagnostics\n"
        "controlled loader output\ncontrolled loader diagnostic\n"
        "controlled loader output\ncontrolled loader diagnostic\n"
    )
    assert not list(Path(session["directory"]).rglob("*.log"))
    assert not image.exists()
    assert not (Path(session["directory"]) / "fdl1.bin").exists()
    assert not (Path(session["directory"]) / "maps").exists()
    assert Path(session["private_key"]).exists()


def test_two_boots_retain_one_ready_session_and_failed_next_boot_preserves_its_keys(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Execute real session, loader and SSH processes with controlled native USB/ACL state."""
    archive = tmp_path / "archive"
    archive.mkdir()
    bundle = session_preparation_bundle(archive)
    for role, source in {
        "loader": "tests/fixtures/processes/windows_loader_output.py",
        "bridge": "tests/fixtures/processes/windows_loader_output.py",
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

        def ps(self, script: str, *arguments: str, timeout: float = 20) -> dict[str, Any]:
            del arguments, timeout
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
        archive, capsule, events=host.progress, log=archive / "logs/first.log"
    )
    first_directory = Path(first["directory"])
    _transport, second = windows_host.boot_archive(
        archive, capsule, events=host.progress, log=archive / "logs/second.log"
    )

    assert first["usb_serial"] != second["usb_serial"]
    assert not first_directory.exists()
    second_directory = Path(second["directory"])
    assert list((archive / "data/session").glob("*/session.json")) == [
        second_directory / "session.json"
    ]
    assert not (second_directory / "image.bin").exists()
    assert not (second_directory / "maps").exists()
    private_key = Path(second["private_key"]).read_bytes()
    known_hosts = Path(second["known_hosts"]).read_bytes()
    monkeypatch.setenv("FPLINUX_TEST_LOADER_STATUS", "7")

    with pytest.raises(SystemExit, match="failed with status 7"):
        windows_host.boot_archive(
            archive, capsule, events=host.progress, log=archive / "logs/failed.log"
        )

    assert Path(second["private_key"]).read_bytes() == private_key
    assert Path(second["known_hosts"]).read_bytes() == known_hosts
    assert list((archive / "data/session").glob("*/session.json")) == [
        second_directory / "session.json"
    ]
    assert unrelated.read_text() == "unrelated user data\n"
    assert capsule.read_bytes() == b"controlled capsule"


def test_powershell_discovery_timeout_reaps_the_owned_process(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Production discovery timeout cleans up a real native-command stand-in."""
    pid_file = tmp_path / "powershell.pid"
    monkeypatch.setenv("FPLINUX_TEST_DISCOVERY_PID", str(pid_file))
    powershell = tmp_path / "powershell.exe"
    install_python_script(
        ROOT / "tests/fixtures/processes/windows_discovery_timeout.py", powershell
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
        ROOT / "tests/fixtures/processes/windows_powershell_boundary.py", powershell
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
        ROOT / "tests/fixtures/processes/windows_powershell_boundary.py", powershell
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


@pytest.mark.parametrize(
    ("script", "response"),
    [
        ("driver-consent.ps1", {"accepted": True}),
        ("driver-elevate.ps1", {"cancelled": False, "status": 0}),
    ],
)
def test_driver_dialog_carrier_has_no_console_and_keeps_script_arguments(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, script: str, response: dict[str, object]
) -> None:
    """Inspect native launch policy; Linux cannot prove WinForms or UAC visibility."""
    arguments_file = tmp_path / "arguments.json"
    monkeypatch.setenv("FPLINUX_TEST_POWERSHELL_ARGUMENTS", str(arguments_file))
    monkeypatch.setenv("FPLINUX_TEST_POWERSHELL_RESULT", json.dumps(response))
    powershell = tmp_path / "powershell.exe"
    install_python_script(
        ROOT / "tests/fixtures/processes/windows_powershell_boundary.py", powershell
    )
    host = windows_host.WindowsHost.__new__(windows_host.WindowsHost)
    host.powershell = powershell

    assert host.ps(script, timeout=3) == response

    assert json.loads(arguments_file.read_text(encoding="utf-8")) == [
        "-NoLogo",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(ROOT / "common/windows" / script),
    ]
