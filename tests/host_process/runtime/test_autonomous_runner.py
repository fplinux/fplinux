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
from tests.fixtures.portable_archive import capsule_files
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

    from tests.fixtures.portable_archive import PortableArchive

portable_archive = archive_fixtures.portable_archive


@dataclass(frozen=True)
class AutonomousRunnerArchive:
    """Own an isolated delivery with real launcher and runner code."""

    root: Path

    def environment(self) -> dict[str, str]:
        """Keep every controlled executable and USB endpoint inside this archive."""
        return {
            "PATH": str(self.root / "bin"),
            "LANG": "C.UTF-8",
            "PYTHONDONTWRITEBYTECODE": "1",
            "FPLINUX_AUTONOMOUS_TEST_ROOT": str(self.root),
        }

    def run(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Enter the delivered launcher with local terminal input and owned tool paths."""
        return run_process(
            [sys.executable, "-I", str(self.root / "terminal.py"), *arguments],
            name="autonomous archive runner",
            cwd=self.root.parent,
            env=self.environment(),
            timeout=30,
        )

    def run_direct(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Execute the real fixed-bundle CLI with no terminal or checkout imports."""
        return run_process(
            [sys.executable, "-I", str(self.root / "fplinux.py"), *arguments],
            name="noninteractive archive command",
            cwd=self.root.parent,
            env=self.environment(),
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
    runtime["personalization"]["template_sha256"] = hashlib.sha256(bytes(512)).hexdigest()
    for device in runtime["usb"].values():
        device["wait_seconds"] = 2
    runtime["adapter"].update({"handoff_wait_seconds": 2, "usb_release_wait_seconds": 2})
    for relative in runtime["sha256"]:
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
    (root / "image/ramboot.bin").write_bytes(b"DHTB" + b"p" * 1020 + bytes(512) + b"suffix")
    (root / "60-fplinux.rules").write_bytes(b'SUBSYSTEM=="usb", MODE="0660"\n')
    peer_root = ROOT / "tests/host_process/runtime/fixtures/autonomous-runner"
    shutil.copyfile(peer_root / "autonomous_terminal.py", root / "terminal.py")
    discovery = (peer_root / "autonomous_usb_boundary.py").read_text(encoding="utf-8")
    for relative in (
        "platforms/ums9117/host/adapter.py",
        "scripts/fplinux_cli/runtime/ssh_transport.py",
    ):
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

    result = archive.run()

    assert result.returncode == 0, result.stderr
    received = archive.root / "boundary/bridge-received"
    assert capsule_files((received / "device-data.cpio").read_bytes()) == {
        "lib/firmware/chip/radio.bin": b"radio",
        "lib/firmware/chip/fm.bin": b"fm",
        "lib/firmware/fplinux/audio.bin": b"profile",
    }
    assert not (received / "pinmap.bin").exists()
    assert not (received / "keymap.bin").exists()
    assert not (archive.root / "data/phone/device-data.cpio").exists()
    assert not list((archive.root / "data").glob(".boot-*"))
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
    log = max((archive.root / "logs").rglob("*.log")).read_text()
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


def test_first_run_rejects_a_corrupted_ram_image_before_privileged_usb_setup(
    autonomous_runner_archive: AutonomousRunnerArchive,
) -> None:
    """A new archive's backup boot must admit its image before installing host rules."""
    archive = autonomous_runner_archive
    shutil.rmtree(archive.root / "data/phone")
    image = archive.root / "image/ramboot.bin"
    contents = bytearray(image.read_bytes())
    contents[-1] ^= 1
    image.write_bytes(contents)

    result = archive.run()

    assert result.returncode == 1
    assert "runtime closure differs from its immutable build manifest" in result.stderr
    assert not (archive.root / "boundary/usb-access.rules").exists()
    assert not (archive.root / "boundary/rules-reloaded").exists()
    assert not (archive.root / "boundary/ram-loads.txt").exists()
    assert not (archive.root / "data/nand.bin").exists()
    log = max((archive.root / "logs").rglob("*.log")).read_text()
    assert "usb-access-setup" not in log
    assert "Setting up USB access" not in result.stdout + log


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


def test_explicit_repreparation_refuses_the_previous_live_phone_without_another_load(
    live_runner_archive: AutonomousRunnerArchive, tmp_path: Path
) -> None:
    """Prepared bytes change through the public preparation action while old RAM stays live."""
    archive = live_runner_archive
    saved_session = archive.shells()[0]
    backup = tmp_path / "another-phone.bin"
    with backup.open("wb") as stream:
        stream.truncate(138412032)
        stream.write(b"other")

    prepared = archive.run_direct("prepare", "--replace", "--from-dump", str(backup))
    assert prepared.returncode == 0, prepared.stderr
    assert (archive.root / "data/phone/groups/bluetooth/radio.bin").read_bytes() == b"other"

    stopped = archive.run()

    assert stopped.returncode == 1
    assert "different runtime or prepared phone data" in stopped.stderr
    assert archive.shells() == [saved_session]
    assert (archive.root / "boundary/ram-loads.txt").read_text() == "RAM load\n"


def test_explicit_remote_command_stdout_contains_only_the_remote_bytes(
    live_runner_archive: AutonomousRunnerArchive,
) -> None:
    """The actual entrypoint keeps runtime progress off the remote command's stdout."""
    archive = live_runner_archive

    result = archive.run_direct("run", "--reconnect", "--exec", "printf ready")

    assert result.returncode == 0, result.stderr
    assert result.stdout == "ready"
    assert "Log:" in result.stderr
    assert (archive.root / "boundary/ram-loads.txt").read_text() == "RAM load\n"


def test_explicit_reconnect_shell_requires_a_terminal_without_starting_another_boot(
    live_runner_archive: AutonomousRunnerArchive,
) -> None:
    """The delivered CLI rejects an explicit noninteractive shell after authenticating."""
    archive = live_runner_archive
    saved_session = archive.shells()[0]

    result = archive.run_direct("run", "--reconnect")

    assert result.returncode == 1
    assert "interactive SSH requires a terminal" in result.stderr
    assert archive.shells() == [saved_session]
    assert (archive.root / "boundary/ram-loads.txt").read_text() == "RAM load\n"


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
