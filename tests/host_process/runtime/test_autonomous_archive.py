# SPDX-License-Identifier: GPL-2.0-only
"""Run the delivered archive with real backup/extraction and a controlled phone boundary.

The RAM loader and remote transport are replaced. Runtime admission, raw backup,
group extraction, selected generations and capsule artifacts remain real. These
cases do not establish phone loading or native Windows behavior.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.bundle_support import file_record
from tests.small.device_data import test_portable
from tests.windows_host_support import archive_files

if TYPE_CHECKING:
    from pathlib import Path

    from tests.small.device_data.test_portable import PortableArchive


saved_dump_archive = test_portable.portable_archive


@dataclass(frozen=True)
class AutonomousArchive:
    """The delivered entry point and its controlled raw reader input."""

    prepared: PortableArchive

    @property
    def root(self) -> Path:
        """Return the owned delivery directory."""
        return self.prepared.root

    def run(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Execute Run.py in isolation from the source checkout and caller directory."""
        return self.prepared.run_entrypoint("Run.py", *arguments)


@pytest.fixture
def autonomous_archive(saved_dump_archive: PortableArchive) -> AutonomousArchive:
    """Combine real portable extraction with canonical admission and a fake phone."""
    archive = saved_dump_archive
    root = archive.root
    archive_files(root)
    (root / "windows-host.json").unlink()
    runner = root / "runner/run.py"
    with runner.open("a", encoding="utf-8") as stream:
        stream.write(
            "\n" + (ROOT / "tests/fixtures/processes/autonomous_phone_backend.py").read_text()
        )
    (root / "tools/spd_dump.exe").chmod(0o755)
    (root / "tools/libc_server.exe").chmod(0o755)
    shutil.copyfile(ROOT / "common/autonomous.py", root / "Run.py")
    (root / "phone-backup-path").write_text(str(archive.dump), encoding="utf-8")
    archive.manifest["nand"]["raw_device"] = "/dev/raw-nand"
    (root / "device-data-manifest.json").write_text(json.dumps(archive.manifest), encoding="utf-8")
    runtime_path = root / "runtime-manifest.json"
    runtime = json.loads(runtime_path.read_text())
    runtime["target"] = "nokia-ta1618"
    runtime["identity"]["target"] = {
        "brand": "Nokia",
        "product": "105 4G",
        "hardware_codes": ["TA-1618"],
        "compatible": "nokia,ta1618",
        "display_name": "Nokia 105 4G (TA-1618)",
    }
    runtime["adapter"]["boot_instructions"] = "Connect the powered-off phone while holding *."
    runtime_path.write_text(json.dumps(runtime), encoding="utf-8")
    build_path = root / "build-manifest.json"
    build = json.loads(build_path.read_text())
    build.pop("generation")
    build["target"] = "nokia-ta1618"
    build["files"]["runtime-manifest.json"] = file_record(runtime_path)
    payload = (json.dumps(build, sort_keys=True, separators=(",", ":")) + "\n").encode()
    build["generation"] = hashlib.sha256(payload).hexdigest()
    build_path.write_text(json.dumps(build), encoding="utf-8")
    return AutonomousArchive(archive)


def _capsule_files(capsule: Path, destination: Path) -> dict[str, bytes]:
    executable = shutil.which("cpio")
    if executable is None:
        pytest.skip("GNU cpio is required for capsule artifact inspection")
    destination.mkdir()
    extracted = subprocess.run(
        [executable, "--extract", "--make-directories", "--no-preserve-owner"],
        input=capsule.read_bytes(),
        cwd=destination,
        capture_output=True,
        check=False,
    )
    assert extracted.returncode == 0, extracted.stderr.decode()
    return {
        path.relative_to(destination).as_posix(): path.read_bytes()
        for path in destination.rglob("*")
        if path.is_file()
    }


def test_first_run_saves_and_prepares_then_same_entry_boots_personal_data(
    autonomous_archive: AutonomousArchive, tmp_path: Path
) -> None:
    archive = autonomous_archive
    first = archive.run()

    assert first.returncode == 0, first.stderr
    assert "Nokia 105 4G (TA-1618)" in first.stdout
    assert "Internal storage is read only." in first.stdout
    assert "Connect the powered-off phone while holding *." in first.stdout
    assert "Checking the computer and USB access..." in first.stdout
    assert "Choosing the private USB connection..." in first.stdout
    assert "Creating secure access for this boot..." in first.stdout
    assert "Preparing the RAM image..." in first.stdout
    assert "Phone detected..." in first.stdout
    assert "Loading the phone into RAM; keep USB connected..." in first.stdout
    assert "Starting Linux..." in first.stdout
    assert "Secure phone connection is ready." in first.stdout
    assert "Run the same script again with the phone powered off." in first.stdout
    assert "sha256=" not in first.stdout
    assert "private backend details" not in first.stdout
    log = next((archive.root / "logs").glob("*.log")).read_text(encoding="utf-8")
    assert log.index("private backend details") < log.index("native loader details")
    saved = archive.root / "data/nand.bin"
    with saved.open("rb") as stream:
        saved_digest = hashlib.file_digest(stream, "sha256").hexdigest()
    with archive.prepared.dump.open("rb") as stream:
        assert saved_digest == hashlib.file_digest(stream, "sha256").hexdigest()
    loaded = archive.root / "data/loaded"
    assert _capsule_files(loaded / "1.cpio", tmp_path / "first-capsule") == {}
    assert not (loaded / "2.cpio").exists()
    readers = (loaded / "reader-commands").read_text()
    assert readers.splitlines() == [
        "cat /sys/class/misc/raw-nand/device/geometry 3</dev/raw-nand",
        "exec dd if=/dev/raw-nand bs=63360",
    ]
    generation = archive.root / "data/phone"
    assert not (generation / "originals").exists()
    assert not (generation / "source/nand.bin").exists()
    assert "Reading NAND:" in first.stdout
    assert "100.0%" in first.stdout

    second = archive.run()

    assert second.returncode == 0, second.stderr
    assert "Opening SSH." in second.stdout
    assert "SSH terminal closed." in second.stdout
    assert "Saving a complete backup" not in second.stdout
    assert "Preparing your phone data" not in second.stdout
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (loaded / "shell-opened").read_text() == "opened\n"
    assert len(list((archive.root / "logs").glob("*.log"))) == 2
    assert all(
        "interactive shell output" not in path.read_text()
        for path in (archive.root / "logs").glob("*.log")
    )
    assert (loaded / "reader-commands").read_text() == readers
    assert _capsule_files(loaded / "2.cpio", tmp_path / "second-capsule") == {
        "lib/firmware/chip/radio.bin": b"radio",
        "lib/firmware/chip/fm.bin": b"fm",
        "lib/firmware/fplinux/audio.bin": b"profile",
    }
    assert (loaded / "2-pinmap.bin").read_bytes() == bytes.fromhex(
        "b0002a40 10000000 ffffffff ffffffff"
    )
    assert (loaded / "2-keymap.bin").read_bytes() == b"\x2a\x00\x31\x00"

    reconnected = archive.run("--ssh")

    assert reconnected.returncode == 0, reconnected.stderr
    assert "Connecting to the running phone..." in reconnected.stdout
    assert "Opening SSH." in reconnected.stdout
    assert not (loaded / "3.cpio").exists()
    assert (loaded / "shell-opened").read_text() == "opened\nopened\n"
    assert (loaded / "reader-commands").read_text() == readers
    assert len(list((archive.root / "logs").glob("*.log"))) == 3


def test_saved_dump_prepares_without_booting_or_replacing_the_input(
    autonomous_archive: AutonomousArchive,
) -> None:
    archive = autonomous_archive
    result = archive.run("--from-dump", str(archive.prepared.dump))

    assert result.returncode == 0, result.stderr
    assert "Your phone data is ready in data/phone." in result.stdout
    assert not (archive.root / "data/loaded").exists()
    assert not (archive.root / "data/nand.bin").exists()
    with archive.prepared.dump.open("rb") as stream:
        assert stream.read(5) == b"radio"
    generation = archive.root / "data/phone"
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (generation / "device-data.cpio").is_file()


def test_target_selection_is_absent_from_the_archive_entry(
    autonomous_archive: AutonomousArchive,
) -> None:
    archive = autonomous_archive
    result = archive.run("--target", "unregistered-phone")

    assert result.returncode == 2
    assert "unrecognized arguments: --target unregistered-phone" in result.stderr
    assert not (archive.root / ".cache").exists()


def test_unusable_saved_dump_reports_the_owned_diagnostic_log(
    autonomous_archive: AutonomousArchive,
) -> None:
    archive = autonomous_archive
    archive.prepared.dump.write_bytes(b"incomplete backup")
    result = archive.run("--from-dump", str(archive.prepared.dump))

    assert result.returncode == 1
    assert "expected a complete 138412032-byte physical NAND backup" in result.stderr
    log = next((archive.root / "logs").glob("*.log"))
    assert str(log) in result.stderr
    assert not (archive.root / "data/loaded").exists()


def test_startup_timeout_reports_the_log_without_exposing_the_command(
    autonomous_archive: AutonomousArchive,
) -> None:
    """A timed-out host boundary stays a concise delivered-entry failure."""
    archive = autonomous_archive
    (archive.root / "phone-startup-timeout").touch()

    result = archive.run()

    assert result.returncode == 1
    assert "Checking the computer and USB access..." in result.stdout
    assert "timed out" in result.stderr
    assert "20" in result.stderr
    assert "logs/" in result.stderr
    visible = result.stdout + result.stderr
    for private_detail in (
        "Traceback",
        "TimeoutExpired",
        "EncodedCommand",
        "private-command-payload",
    ):
        assert private_detail not in visible
    log = next((archive.root / "logs").glob("*.log")).read_text()
    assert "TimeoutExpired" in log
    assert "private-command-payload" in log
    assert "Connect the powered-off phone" not in result.stdout
    assert not (archive.root / "data/loaded").exists()
    assert not (archive.root / "data/nand.bin").exists()


def test_prefixed_host_failure_reports_one_prefix_and_retains_the_log(
    autonomous_archive: AutonomousArchive,
) -> None:
    """The delivered entry preserves a host failure without duplicating its prefix."""
    archive = autonomous_archive
    (archive.root / "phone-startup-failure").write_text(
        "FPLinux: Session path is not private to the current Windows account: client-key.pub\n",
        encoding="utf-8",
    )

    result = archive.run()

    assert result.returncode == 1
    assert "Checking the computer and USB access..." in result.stdout
    log_path = next((archive.root / "logs").glob("*.log"))
    assert result.stderr == (
        "FPLinux: Session path is not private to the current Windows account: client-key.pub\n"
        f"See {log_path} for details.\n"
    )
    log = log_path.read_text(encoding="utf-8")
    assert "SystemExit: FPLinux: Session path is not private" in log
    assert "Connect the powered-off phone" not in result.stdout
    assert not (archive.root / "data/loaded").exists()
    assert not (archive.root / "data/nand.bin").exists()


def test_cancelled_loader_preserves_signal_status_and_reports_cancellation(
    autonomous_archive: AutonomousArchive,
) -> None:
    """The external loader boundary reports its owned SIGINT result to the real entry."""
    archive = autonomous_archive
    (archive.root / "phone-startup-cancel").touch()

    result = archive.run()

    assert result.returncode == 130
    assert "Cancelled." in result.stdout
    assert "FPLinux: 130" not in result.stderr
    assert "Traceback" not in result.stderr
    assert not (archive.root / "data/nand.bin").exists()
    assert not list((archive.root / "data").glob(".initial-boot-*"))
