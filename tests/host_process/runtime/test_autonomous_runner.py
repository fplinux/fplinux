# SPDX-License-Identifier: GPL-2.0-only
"""Run the archive launcher, runner, adapter and saved-session lifecycle together.

Local processes replace the RAM tools, SSH peer and privileged USB-rule commands;
controlled discovery replaces sysfs and NCM readiness. These host cases establish
archive data forwarding and reconnect decisions, not phone or network behavior.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

import pytest

from tests import ROOT
from tests.bundle_support import file_record
from tests.fixtures import portable_archive as archive_fixtures
from tests.fixtures.executables import install_python_script
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

    from tests.fixtures.portable_archive import PortableArchive

portable_archive = archive_fixtures.portable_archive


@dataclass(frozen=True)
class AutonomousRunnerArchive:
    """Own an isolated delivery with real launcher and runner code."""

    root: Path

    def run(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Enter the delivered launcher with local terminal input and owned tool paths."""
        environment = {
            "PATH": str(self.root / "bin"),
            "LANG": "C.UTF-8",
            "PYTHONDONTWRITEBYTECODE": "1",
            "FPLINUX_AUTONOMOUS_TEST_ROOT": str(self.root),
        }
        return run_process(
            [sys.executable, "-I", str(self.root / "terminal.py"), *arguments],
            name="autonomous archive runner",
            cwd=self.root.parent,
            env=environment,
            timeout=30,
        )

    def shells(self) -> list[dict[str, Any]]:
        """Read the saved session actually received by each separate SSH shell process."""
        return [
            json.loads(line)
            for line in (self.root / "boundary/shells.jsonl").read_text().splitlines()
        ]


def _refresh_archive_closure(root: Path) -> None:
    """Declare the controlled delivery bytes for the real admission consumers."""
    runtime_path = root / "runtime-manifest.json"
    runtime = json.loads(runtime_path.read_text(encoding="utf-8"))
    runtime["assets"].update({"pinmap": "assets/pinmap.bin", "keymap": "assets/keymap.bin"})
    runtime["personalization"]["template_sha256"] = hashlib.sha256(bytes(512)).hexdigest()
    for device in runtime["usb"].values():
        device["wait_seconds"] = 2
    runtime["adapter"].update({"handoff_wait_seconds": 2, "usb_release_wait_seconds": 2})
    for relative in (*runtime["sha256"], "assets/pinmap.bin", "assets/keymap.bin"):
        runtime["sha256"][relative] = hashlib.sha256((root / relative).read_bytes()).hexdigest()
    runtime_path.write_text(json.dumps(runtime), encoding="utf-8")
    build_path = root / "build-manifest.json"
    build = json.loads(build_path.read_text(encoding="utf-8"))
    build.pop("generation")
    for relative in ("runtime-manifest.json", runtime["image"]):
        build["files"][relative] = file_record(root / relative)
    canonical = (json.dumps(build, sort_keys=True, separators=(",", ":")) + "\n").encode()
    build["generation"] = hashlib.sha256(canonical).hexdigest()
    build_path.write_text(json.dumps(build), encoding="utf-8")


@pytest.fixture
def autonomous_runner_archive(portable_archive: PortableArchive) -> AutonomousRunnerArchive:
    """Prepare phone data, then replace only external discovery and tool boundaries."""
    archive = portable_archive
    prepared = archive.run("--from-dump", str(archive.dump))
    assert prepared.returncode == 0, prepared.stderr
    root = archive.root
    boundary = root / "boundary"
    boundary.mkdir()
    (boundary / "bootrom").touch()
    (root / "assets/pinmap.bin").write_bytes(b"bundle fallback pinmap")
    (root / "assets/keymap.bin").write_bytes(b"bundle fallback keymap")
    (root / "image/ramboot.bin").write_bytes(b"DHTB" + b"p" * 1020 + bytes(512) + b"suffix")
    (root / "60-fplinux.rules").write_bytes(b'SUBSYSTEM=="usb", MODE="0660"\n')
    peer_root = ROOT / "tests/host_process/runtime/fixtures/autonomous-runner"
    shutil.copyfile(peer_root / "autonomous_terminal.py", root / "terminal.py")
    discovery = (peer_root / "autonomous_usb_boundary.py").read_text(encoding="utf-8")
    for relative in ("runner/platform_adapter.py", "runner/ssh_transport.py"):
        with (root / relative).open("a", encoding="utf-8") as stream:
            stream.write("\n" + discovery)
    tools = root / "bin"
    tools.mkdir()
    for relative in (
        "tools/spd_dump.exe",
        "tools/libc_server.exe",
        "bin/ip",
        "bin/ssh",
        "bin/ssh-keyscan",
        "bin/sudo",
        "bin/udevadm",
    ):
        install_python_script(peer_root / "autonomous_runner_tool.py", root / relative)
    runtime_tools = ROOT / "tests/fixtures/runtime-tools"
    install_python_script(runtime_tools / "windows_keygen.py", tools / "ssh-keygen")
    shutil.copyfile(runtime_tools / "stdbuf.sh", tools / "stdbuf")
    (tools / "stdbuf").chmod(0o755)
    _refresh_archive_closure(root)
    return AutonomousRunnerArchive(root)


def test_prepared_phone_data_reaches_bridge_with_owned_log_and_archive_session(
    autonomous_runner_archive: AutonomousRunnerArchive,
) -> None:
    """Real orchestration forwards prepared data and performs the controlled USB setup."""
    archive = autonomous_runner_archive
    expected_capsule = (archive.root / "data/phone/device-data.cpio").read_bytes()

    result = archive.run()

    assert result.returncode == 0, result.stderr
    received = archive.root / "boundary/bridge-received"
    assert (received / "device-data.cpio").read_bytes() == expected_capsule
    assert (received / "pinmap.bin").read_bytes() == bytes.fromhex(
        "b0002a40 10000000 ffffffff ffffffff"
    )
    assert (received / "keymap.bin").read_bytes() == bytes.fromhex("2a00 3100")
    assert (archive.root / "boundary/usb-access.rules").read_bytes() == (
        b'SUBSYSTEM=="usb", MODE="0660"\n'
    )
    assert (archive.root / "boundary/rules-reloaded").read_text() == "reloaded\n"
    session_root = archive.root / "data/session"
    session = json.loads((session_root / "current/nokia-ta1618.json").read_text())
    assert session["runtime_root"] == str(session_root)
    assert Path(session["private_key"]).is_relative_to(session_root / "sessions")
    assert Path(session["private_key"]).is_file()
    assert not Path(session["image"]).exists()
    assert (archive.root / "boundary/handoff-session.txt").read_text() == session["session_id"]
    assert archive.shells() == [session]
    log = max((archive.root / "logs").glob("*.log")).read_text()
    for output in (
        "controlled RAM loader stdout",
        "controlled RAM loader stderr",
        "controlled bridge stdout",
        "controlled bridge stderr",
    ):
        assert output in log
        assert output not in result.stdout + result.stderr
    assert "controlled interactive shell output" in result.stdout
    assert "controlled interactive shell output" not in log


@pytest.fixture
def live_runner_archive(
    autonomous_runner_archive: AutonomousRunnerArchive,
) -> AutonomousRunnerArchive:
    """Persist a ready archive session through the real initial runner flow."""
    archive = autonomous_runner_archive
    loaded = archive.run()
    assert loaded.returncode == 0, loaded.stderr
    return archive


def test_live_archive_session_reopens_shell_without_another_ram_load(
    live_runner_archive: AutonomousRunnerArchive,
) -> None:
    """A second real launcher uses the saved archive binding and leaves RAM tools idle."""
    archive = live_runner_archive
    saved_session = archive.shells()[0]
    loaded_image = (archive.root / "boundary/loaded-image.bin").read_bytes()

    result = archive.run()

    assert result.returncode == 0, result.stderr
    assert "Connecting to the running phone..." in result.stdout
    assert "Starting FPLinux with your saved phone data..." not in result.stdout
    assert archive.shells() == [saved_session, saved_session]
    assert (archive.root / "boundary/ram-loads.txt").read_text() == "RAM load\n"
    assert (archive.root / "boundary/loaded-image.bin").read_bytes() == loaded_image


@pytest.mark.parametrize(
    ("mode", "error"),
    [
        ("session-identity", "SSH endpoint proved a different RAM session identity"),
        ("kernel-identity", "current SSH session exposes a different kernel identity"),
        ("ambiguous-endpoint", "more than one USB device matches this RAM session"),
    ],
)
def test_reconnect_identity_or_endpoint_failure_stops_without_fallback_boot(
    live_runner_archive: AutonomousRunnerArchive, mode: str, error: str
) -> None:
    """Real reconnect decisions reject controlled unsafe endpoints before a shell or RAM load."""
    archive = live_runner_archive
    saved_session = archive.shells()[0]
    (archive.root / "boundary/reconnect-mode").write_text(mode, encoding="ascii")

    result = archive.run()

    assert result.returncode == 1
    assert error in result.stderr
    assert "Starting FPLinux with your saved phone data..." not in result.stdout
    assert "Connect the powered-off phone" not in result.stdout
    assert archive.shells() == [saved_session]
    assert (archive.root / "boundary/ram-loads.txt").read_text() == "RAM load\n"
