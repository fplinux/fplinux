# SPDX-License-Identifier: GPL-2.0-only
"""Run the delivered archive with real backup/extraction and a controlled phone boundary.

The RAM loader and remote transport are replaced. Runtime admission, raw backup,
group extraction, prepared phone data and capsule artifacts remain real. These
cases do not establish phone loading or native Windows behavior.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import signal
import subprocess
import sys
import time
from contextlib import contextmanager
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.fixtures.portable_archive import (
    portable_archive as portable_archive,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.fixtures.portable_archive import (
    refresh_archive,
)

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

    from tests.fixtures.portable_archive import PortableArchive


@dataclass(frozen=True)
class AutonomousArchive:
    """The delivered entry point and its controlled raw reader input."""

    prepared: PortableArchive

    @property
    def root(self) -> Path:
        """Return the owned delivery directory."""
        return self.prepared.root

    @property
    def data(self) -> Path:
        """Return this installation's mutable runtime scope."""
        return self.prepared.runtime_data

    @property
    def logs(self) -> Path:
        """Return launch journals within the runtime scope."""
        return self.prepared.logs

    @property
    def phone(self) -> Path:
        """Return prepared data in the shared target store."""
        return self.prepared.prepared_data()

    @property
    def backup(self) -> Path:
        """Return the complete backup in the shared target store."""
        return self.prepared.phone_store() / "nand.bin"

    def run(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Execute the fixed bundle runtime without checkout imports or its working directory."""
        return self.prepared.run_entrypoint("terminal.py", *arguments)

    @contextmanager
    def running(self) -> Iterator[subprocess.Popen[str]]:
        """Own a real launcher process while a controlled phone boundary waits."""
        with subprocess.Popen(
            [sys.executable, "-I", str(self.root / "terminal.py")],
            cwd=self.root.parent,
            env=self.prepared.environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        ) as process:
            try:
                yield process
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=10)


def _wait_for_file(path: Path, process: subprocess.Popen[str]) -> None:
    deadline = time.monotonic() + 10
    while not path.exists():
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            pytest.fail(f"Launcher exited before {path.name}: {stdout}\n{stderr}")
        if time.monotonic() >= deadline:
            pytest.fail(f"Launcher did not reach {path.name} before the deadline")
        time.sleep(0.02)


@pytest.fixture
def autonomous_archive(portable_archive: PortableArchive) -> AutonomousArchive:
    """Combine real portable extraction with canonical admission and a fake phone."""
    archive = portable_archive
    root = archive.root
    runner = root / "scripts/fplinux_cli/runtime/boot.py"
    with runner.open("a", encoding="utf-8") as stream:
        stream.write(
            "\n"
            + (
                ROOT
                / "tests/host_process/runtime/fixtures/autonomous-archive"
                / "autonomous_phone_backend.py"
            ).read_text()
        )
    (root / "phone-backup-path").write_text(str(archive.dump), encoding="utf-8")
    (root / "phone-prepared-path").write_text(str(archive.prepared_data()), encoding="utf-8")
    shutil.copyfile(
        ROOT / "tests/fixtures/nand-geometry/autonomous.txt", root / "phone-geometry.txt"
    )
    shutil.copyfile(
        ROOT / "tests/host_process/runtime/fixtures/autonomous-runner/autonomous_terminal.py",
        root / "terminal.py",
    )
    refresh_archive(root)
    return AutonomousArchive(archive)


def _capsule_files(capsule: Path, destination: Path) -> dict[str, bytes]:
    executable = shutil.which("cpio")
    if executable is None:
        pytest.fail("GNU cpio is required for capsule artifact inspection")
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


def test_first_run_with_ram_squashfs_and_zram_prepares_then_same_entry_boots_personal_data(
    autonomous_archive: AutonomousArchive, tmp_path: Path
) -> None:
    archive = autonomous_archive
    (archive.root / "phone-measured-progress").touch()
    first = archive.run()

    assert first.returncode == 0, first.stderr
    assert "Nokia 105 4G (TA-1618)" in first.stdout
    assert "Connect the powered-off phone while holding *." in first.stdout
    assert "Checking the computer and USB access..." in first.stdout
    assert "Preparing the phone's USB network connection..." in first.stdout
    assert "Creating SSH keys for this boot..." in first.stdout
    assert "Preparing the RAM image..." in first.stdout
    assert "Phone detected..." in first.stdout
    assert "Loading the phone into RAM; keep USB connected..." in first.stdout
    assert "Starting Linux..." in first.stdout
    assert "Secure phone connection is ready." in first.stdout
    assert "Run the same launcher again with the phone powered off." in first.stdout
    assert "sha256=" not in first.stdout
    assert "private backend details" not in first.stdout
    log = next(archive.logs.rglob("*.log")).read_text(encoding="utf-8")
    assert log.index("private backend details") < log.index("native loader details")
    saved = archive.backup
    with saved.open("rb") as stream:
        saved_digest = hashlib.file_digest(stream, "sha256").hexdigest()
    with archive.prepared.dump.open("rb") as stream:
        assert saved_digest == hashlib.file_digest(stream, "sha256").hexdigest()
    loaded = archive.data / "loaded"
    assert _capsule_files(loaded / "1.cpio", tmp_path / "first-capsule") == {
        "etc/fplinux/preparation": b"1\n"
    }
    stages = [
        json.loads(line) for line in (loaded / "preparation-stages").read_text().splitlines()
    ]
    assert [entry["arguments"] for entry in stages] == [
        ["--stage", "waiting", "/run/fplinux-preparation-status"],
        [
            "--stage",
            "reading",
            "--completed",
            "0",
            "--total",
            "138412032",
            "/run/fplinux-preparation-status",
        ],
        [
            "--stage",
            "reading",
            "--completed",
            "69206016",
            "--total",
            "138412032",
            "/run/fplinux-preparation-status",
        ],
        [
            "--stage",
            "reading",
            "--completed",
            "138412032",
            "--total",
            "138412032",
            "/run/fplinux-preparation-status",
        ],
        ["--stage", "extracting", "/run/fplinux-preparation-status"],
        ["--stage", "ready", "/run/fplinux-preparation-status"],
    ]
    assert all(not entry["phone_data"] for entry in stages[:-1])
    assert all(not entry["storage_checked"] for entry in stages[:-1])
    assert stages[-1]["phone_data"] is True
    assert stages[-1]["storage_checked"] is True
    assert not (loaded / "2.cpio").exists()
    readers = (loaded / "reader-commands").read_text()
    assert readers.splitlines() == [
        "cat /sys/class/misc/raw-nand/device/geometry 3</dev/raw-nand",
        "exec dd if=/dev/raw-nand bs=63360",
    ]
    generation = archive.phone
    assert not (generation / "originals").exists()
    assert not (generation / "source/nand.bin").exists()
    assert "Reading NAND:" in first.stdout
    assert "100.0%" in first.stdout
    assert not (loaded / "shell-opened").exists()
    assert not (archive.data / "session").exists()
    (archive.root / "phone-live").unlink()

    second = archive.run()

    assert second.returncode == 0, second.stderr
    assert "Opening SSH." in second.stdout
    assert "SSH terminal closed." in second.stdout
    assert "Saving a complete backup" not in second.stdout
    assert "Preparing your phone data" not in second.stdout
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (loaded / "shell-opened").read_text() == "opened\n"
    assert (archive.data / "session/session.json").exists()
    assert len(list(archive.logs.rglob("*.log"))) == 2
    assert all(
        "interactive shell output" not in path.read_text() for path in archive.logs.rglob("*.log")
    )
    assert (loaded / "reader-commands").read_text() == readers
    assert [
        json.loads(line) for line in (loaded / "preparation-stages").read_text().splitlines()
    ] == stages
    assert _capsule_files(loaded / "2.cpio", tmp_path / "second-capsule") == {
        "lib/firmware/chip/radio.bin": b"radio",
        "lib/firmware/chip/fm.bin": b"fm",
        "lib/firmware/fplinux/audio.bin": b"profile",
    }
    assert not list(loaded.glob("*pinmap*"))
    assert not list(loaded.glob("*keymap*"))
    assert (loaded / "storage-checks").read_text() == "reader keys present\n"

    reconnected = archive.run()

    assert reconnected.returncode == 0, reconnected.stderr
    assert "Connecting to the running phone..." in reconnected.stdout
    assert "Opening SSH." in reconnected.stdout
    assert not (loaded / "3.cpio").exists()
    assert (loaded / "shell-opened").read_text() == "opened\nopened\n"
    assert (loaded / "reader-commands").read_text() == readers
    assert len(list(archive.logs.rglob("*.log"))) == 3


def test_explicit_prepare_imports_saved_dump_without_booting_or_replacing_the_input(
    autonomous_archive: AutonomousArchive,
) -> None:
    archive = autonomous_archive
    result = archive.prepared.run_entrypoint(
        "fplinux.py", "prepare", "--from-dump", str(archive.prepared.dump)
    )

    assert result.returncode == 0, result.stderr
    assert "Your phone data is ready in" in result.stdout
    assert not (archive.data / "loaded").exists()
    assert not (archive.root / "phone-probes").exists()
    assert archive.backup.stat().st_size == 138412032
    with archive.prepared.dump.open("rb") as stream:
        assert stream.read(5) == b"radio"
    generation = archive.phone
    assert (generation / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert not (generation / "device-data.cpio").exists()


@pytest.mark.parametrize("failure", ["missing-helper", "io-error"])
def test_unavailable_phone_progress_warns_once_and_keeps_preparation_successful(
    autonomous_archive: AutonomousArchive,
    failure: str,
) -> None:
    """Optional display feedback cannot turn a complete backup into a failed preparation."""
    archive = autonomous_archive
    (archive.root / "phone-progress-failure").write_text(failure, encoding="utf-8")

    result = archive.run()

    assert result.returncode == 0, result.stderr
    assert result.stdout.count("phone preparation progress is unavailable") == 1
    assert "Your phone data is ready in" in result.stdout
    assert "Run the same launcher again" in result.stdout
    assert archive.backup.stat().st_size == 138412032
    assert (archive.phone / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert not (archive.data / "reader").exists()
    updates = (archive.data / "loaded/preparation-stages").read_text().splitlines()
    assert len(updates) == 1
    assert json.loads(updates[0])["arguments"] == [
        "--stage",
        "waiting",
        "/run/fplinux-preparation-status",
    ]


@pytest.mark.parametrize("progress_failure", [False, True])
def test_failed_nand_read_retains_access_and_its_original_diagnostic(
    autonomous_archive: AutonomousArchive,
    *,
    progress_failure: bool,
) -> None:
    """The failure stage is best effort and cannot replace the actual backup error."""
    archive = autonomous_archive
    (archive.root / "phone-read-failure").touch()
    if progress_failure:
        (archive.root / "phone-progress-failure").write_text("error-stage", encoding="utf-8")

    result = archive.run()

    assert result.returncode == 1
    assert "controlled NAND read failure" in result.stderr
    assert "phone progress connection unavailable" not in result.stderr
    assert (archive.data / "reader/session/session.json").exists()
    assert not archive.backup.exists()
    assert not archive.phone.exists()
    assert not list(archive.data.glob(".nand.bin.*"))
    assert not list(archive.backup.parent.glob(".nand.bin.*"))
    stages = [
        json.loads(line)["arguments"][1]
        for line in (archive.data / "loaded/preparation-stages").read_text().splitlines()
    ]
    assert stages == ["waiting", "reading", "error"]
    assert result.stdout.count("phone preparation progress is unavailable") == int(
        progress_failure
    )


def test_cancelled_nand_read_reports_cancelled_on_phone_and_preserves_reader_access(
    autonomous_archive: AutonomousArchive,
) -> None:
    """A real SIGINT through the launcher leaves a resumable reader and no partial backup."""
    archive = autonomous_archive
    (archive.root / "phone-hold-read").touch()
    with archive.running() as process:
        _wait_for_file(archive.data / "loaded/reader-waiting", process)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=10)

    assert process.returncode == 130, (stdout, stderr)
    assert "Cancelled." in stdout
    assert not stderr
    assert (archive.data / "reader/session/session.json").exists()
    assert not archive.backup.exists()
    assert not archive.phone.exists()
    assert not list(archive.data.glob(".nand.bin.*"))
    assert not list(archive.backup.parent.glob(".nand.bin.*"))
    stages = [
        json.loads(line)["arguments"][1]
        for line in (archive.data / "loaded/preparation-stages").read_text().splitlines()
    ]
    assert stages == ["waiting", "reading", "cancelled"]


def test_invalid_dump_preserves_existing_reader_when_the_phone_is_disconnected(
    autonomous_archive: AutonomousArchive,
    tmp_path: Path,
) -> None:
    """An early reconnect probe must not retire recovery state before extraction succeeds."""
    archive = autonomous_archive
    (archive.root / "phone-read-failure").touch()
    interrupted = archive.run()
    assert interrupted.returncode == 1
    reader = archive.data / "reader/session/session.json"
    saved_keys = reader.read_bytes()
    (archive.root / "phone-live").unlink()
    invalid = tmp_path / "incomplete.bin"
    invalid.write_bytes(b"incomplete physical NAND input")

    result = archive.run("--from-dump", str(invalid))

    assert result.returncode == 1
    assert "expected a complete 138412032-byte physical NAND backup" in result.stderr
    assert reader.read_bytes() == saved_keys
    assert not archive.phone.exists()
    assert len(list((archive.data / "loaded").glob("*.cpio"))) == 1


@pytest.mark.parametrize("storage", ["mounted", "swap", "loop"])
def test_reader_keys_survive_preparation_until_persistent_storage_is_released(
    autonomous_archive: AutonomousArchive,
    storage: str,
) -> None:
    """A retained reader can finish its safety check without another backup or user boot."""
    archive = autonomous_archive
    marker = archive.root / f"phone-storage-{storage}"
    marker.touch()

    first = archive.run()

    assert first.returncode == 1
    assert "Persistent storage is still mounted" in first.stderr
    assert (archive.phone / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (archive.data / "reader/session/session.json").exists()
    stages = [
        json.loads(line)["arguments"][1]
        for line in (archive.data / "loaded/preparation-stages").read_text().splitlines()
    ]
    assert stages[-1] == "error"
    assert "ready" not in stages
    assert not (archive.data / "session").exists()
    reader_commands = (archive.data / "loaded/reader-commands").read_text()
    marker.unlink()

    resumed = archive.run()

    assert resumed.returncode == 0, resumed.stderr
    assert "Disconnect USB." in resumed.stdout
    assert not (archive.data / "reader").exists()
    assert not (archive.data / "session").exists()
    assert not (archive.data / "loaded/shell-opened").exists()
    assert (archive.data / "loaded/reader-commands").read_text() == reader_commands
    assert len(list((archive.data / "loaded").glob("*.cpio"))) == 1
    stages = [
        json.loads(line)["arguments"][1]
        for line in (archive.data / "loaded/preparation-stages").read_text().splitlines()
    ]
    assert stages[-2:] == ["waiting", "ready"]


def test_explicit_reconnect_does_not_retire_or_promote_a_pending_reader(
    autonomous_archive: AutonomousArchive,
) -> None:
    """A completed preparation still needs ordinary reader cleanup before a normal session."""
    archive = autonomous_archive
    mounted = archive.root / "phone-storage-mounted"
    mounted.touch()
    prepared = archive.run()
    assert prepared.returncode == 1
    reader = archive.data / "reader/session/session.json"
    saved = reader.read_bytes()
    mounted.unlink()

    result = archive.prepared.run("--reconnect", "--exec", "printf ready")

    assert result.returncode == 1
    assert "No ready normal phone session" in result.stderr
    assert result.stdout == ""
    assert reader.read_bytes() == saved
    assert not (archive.data / "session").exists()
    assert not (archive.data / "loaded/shell-opened").exists()
    assert len(list((archive.data / "loaded").glob("*.cpio"))) == 1


def test_nand_identify_entrypoint_keeps_progress_off_the_report_stdout(
    autonomous_archive: AutonomousArchive,
) -> None:
    """The shared CLI emits the raw geometry document unchanged after authenticated reconnect."""
    archive = autonomous_archive
    prepared = archive.prepared.run_entrypoint(
        "fplinux.py", "prepare", "--from-dump", str(archive.prepared.dump)
    )
    assert prepared.returncode == 0, prepared.stderr
    booted = archive.run()
    assert booted.returncode == 0, booted.stderr

    result = archive.prepared.run_entrypoint("fplinux.py", "nand", "identify")

    assert result.returncode == 0, result.stderr
    assert result.stdout == (archive.root / "phone-geometry.txt").read_text()
    assert "Log:" in result.stderr
    assert archive.backup.stat().st_size == 138412032


def test_target_selection_is_absent_from_the_archive_entry(
    autonomous_archive: AutonomousArchive,
) -> None:
    archive = autonomous_archive
    result = archive.run("--target", "unregistered-phone")

    assert result.returncode == 2
    assert "unrecognized arguments: --target unregistered-phone" in result.stderr
    assert not (archive.root / ".cache").exists()


def test_corrupt_runtime_identity_is_logged_before_preparation(
    autonomous_archive: AutonomousArchive,
) -> None:
    archive = autonomous_archive
    path = archive.root / "runtime-manifest.json"
    runtime = json.loads(path.read_text())
    runtime["identity"]["target"]["compatible"] = "not canonical"
    path.write_text(json.dumps(runtime), encoding="utf-8")

    result = archive.run("--from-dump", str(archive.prepared.dump))

    assert result.returncode == 1
    assert "compatible" in result.stderr
    journal = json.loads(next(archive.logs.glob("*/run.json")).read_text())
    assert journal["status"] == "failed"
    assert not archive.phone.exists()


def test_unusable_saved_dump_reports_the_owned_diagnostic_log(
    autonomous_archive: AutonomousArchive,
) -> None:
    archive = autonomous_archive
    archive.prepared.dump.write_bytes(b"incomplete backup")
    archive.prepared.dump.with_name(f"{archive.prepared.dump.name}.json").unlink()
    result = archive.run("--from-dump", str(archive.prepared.dump))

    assert result.returncode == 1
    assert "expected a complete 138412032-byte physical NAND backup" in result.stderr
    log = next(archive.logs.rglob("*.log"))
    assert str(log) in result.stderr
    assert not (archive.data / "loaded").exists()


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
    log = next(archive.logs.rglob("*.log")).read_text()
    assert "TimeoutExpired" in log
    assert "private-command-payload" in log
    assert "Connect the powered-off phone" not in result.stdout
    assert not (archive.data / "loaded").exists()
    assert not archive.backup.exists()


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
    log_path = next(archive.logs.rglob("*.log"))
    assert result.stderr == (
        "FPLinux: Session path is not private to the current Windows account: client-key.pub\n"
        f"See {log_path} for details.\n"
    )
    log = log_path.read_text(encoding="utf-8")
    assert "SystemExit: FPLinux: Session path is not private" in log
    assert "Connect the powered-off phone" not in result.stdout
    assert not (archive.data / "loaded").exists()
    assert not archive.backup.exists()


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
    assert not archive.backup.exists()


@pytest.mark.parametrize("boundary", ["loader", "shell"])
@pytest.mark.parametrize("exit_kind", ["normal", "cancel", "crash"])
def test_active_launcher_excludes_another_process_and_releases_its_lock(
    autonomous_archive: AutonomousArchive, boundary: str, exit_kind: str
) -> None:
    """Real Linux process locks cover waiting and SSH; the phone boundary remains controlled."""
    archive = autonomous_archive
    if boundary == "shell":
        prepared = archive.prepared.run_entrypoint(
            "fplinux.py", "prepare", "--from-dump", str(archive.prepared.dump)
        )
        assert prepared.returncode == 0, prepared.stderr
    hold = archive.root / f"phone-hold-{boundary}"
    hold.touch()
    marker = (
        archive.data / "loaded" / ("loader-waiting" if boundary == "loader" else "shell-opened")
    )
    with archive.running() as first:
        _wait_for_file(marker, first)
        logs = sorted(archive.logs.rglob("*.log"))
        capsules = sorted((archive.data / "loaded").glob("*.cpio"))
        probe_path = archive.root / "phone-probes"
        probes = probe_path.read_text() if probe_path.exists() else ""

        excluded = archive.run()

        assert excluded.returncode == 1
        assert "Another launcher is already open." in excluded.stderr
        assert "Connect the powered-off phone" not in excluded.stdout
        assert sorted(archive.logs.rglob("*.log")) == logs
        assert sorted((archive.data / "loaded").glob("*.cpio")) == capsules
        assert (probe_path.read_text() if probe_path.exists() else "") == probes
        if exit_kind == "normal":
            hold.unlink()
        elif exit_kind == "cancel":
            first.send_signal(signal.SIGINT)
        else:
            first.kill()
        stdout, stderr = first.communicate(timeout=60)
        expected_status = {"normal": 0, "cancel": 130, "crash": -signal.SIGKILL}[exit_kind]
        assert first.returncode == expected_status, (stdout, stderr)
        if exit_kind == "cancel":
            assert "Cancelled." in stdout
    hold.unlink(missing_ok=True)
    reopened = archive.run()

    assert reopened.returncode == 0, reopened.stderr
    assert "Another launcher is already open." not in reopened.stderr
    assert (archive.data / ".launcher.lock").is_file()


def test_reconnect_failure_stops_without_starting_another_boot(
    autonomous_archive: AutonomousArchive,
) -> None:
    """The real launcher must preserve the controlled transport's safety failure."""
    error = "SSH endpoint proved a different RAM session identity"
    archive = autonomous_archive
    prepared = archive.prepared.run_entrypoint(
        "fplinux.py", "prepare", "--from-dump", str(archive.prepared.dump)
    )
    assert prepared.returncode == 0, prepared.stderr
    (archive.root / "phone-reconnect-failure").write_text(error, encoding="utf-8")

    stopped = archive.run()

    assert stopped.returncode == 1
    assert error in stopped.stderr
    assert "Starting FPLinux" not in stopped.stdout
    assert "Connect the powered-off phone" not in stopped.stdout
    assert not (archive.data / "loaded").exists()
    assert archive.backup.stat().st_size == 138412032
    (archive.root / "phone-reconnect-failure").unlink()
    reopened = archive.run()
    assert reopened.returncode == 0, reopened.stderr


def test_removed_ssh_mode_is_rejected_before_startup(
    autonomous_archive: AutonomousArchive,
) -> None:
    result = autonomous_archive.run("--ssh")

    assert result.returncode == 2
    assert "unrecognized arguments: --ssh" in result.stderr
    assert not autonomous_archive.logs.exists()
    assert not autonomous_archive.data.exists()


def test_read_only_archive_writes_admission_diagnostics_to_external_data_only(
    portable_archive: PortableArchive, tmp_path: Path
) -> None:
    """Bare Python preserves delivered files while an owned external root records failure."""
    archive = portable_archive
    root = archive.root
    (root / "runtime-manifest.json").write_bytes(b"invalid runtime manifest\n")
    retained = root / "data/session/retained.txt"
    retained.parent.mkdir(parents=True)
    retained.write_text("unselected previous state\n", encoding="utf-8")
    files = [root, *root.rglob("*")]
    original_modes = [(path, path.stat().st_mode & 0o777) for path in files]
    before = {path.relative_to(root).as_posix() for path in files}
    managed = tmp_path / "external data Юникод"
    environment = {**archive.environment(), "FPLINUX_DATA_DIR": str(managed)}
    try:
        for path, mode in original_modes:
            path.chmod(mode & ~0o222)

        result = subprocess.run(
            [sys.executable, "-I", str(root / "fplinux.py"), "run", "--reconnect"],
            cwd=tmp_path,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    finally:
        for path, mode in original_modes:
            path.chmod(mode)

    assert result.returncode == 1, result.stdout + result.stderr
    logs = list((managed / "runtime/archives").glob("*/logs/*/runtime.log"))
    assert len(logs) == 1
    assert str(logs[0]) in result.stdout
    assert {path.relative_to(root).as_posix() for path in [root, *root.rglob("*")]} == before
    assert retained.read_text(encoding="utf-8") == "unselected previous state\n"
    assert not (root / ".data").exists()
    assert not list(root.rglob("__pycache__"))


def test_noninteractive_launch_keeps_the_ready_session_without_opening_a_shell(
    autonomous_archive: AutonomousArchive,
) -> None:
    """The delivered command supports automation while retaining normal reconnect credentials."""
    archive = autonomous_archive
    prepared = archive.prepared.run_entrypoint(
        "fplinux.py", "prepare", "--from-dump", str(archive.prepared.dump)
    )
    assert prepared.returncode == 0, prepared.stderr

    result = archive.prepared.run()

    assert result.returncode == 0, result.stderr
    assert "No interactive terminal is attached" in result.stdout
    assert (archive.data / "session/session.json").exists()
    assert not (archive.data / "loaded/shell-opened").exists()


@pytest.fixture
def sc9820e_archive(autonomous_archive: AutonomousArchive) -> AutonomousArchive:
    """Deliver a direct RAM archive with no NAND preparation modules or manifest."""
    archive = autonomous_archive
    root = archive.root
    runtime_path = root / "runtime-manifest.json"
    runtime = json.loads(runtime_path.read_text(encoding="utf-8"))
    runtime["target"] = "xiaomi-qin-1s-plus"
    runtime["identity"] = {
        "target": {
            "brand": "Xiaomi",
            "product": "Qin 1s+",
            "hardware_codes": [],
            "compatible": "xiaomi,qin1splus",
            "display_name": "Xiaomi Qin 1s+",
        },
        "platform": {
            "name": "sc9820e",
            "vendor": "Unisoc",
            "soc": "SC9820E",
            "aliases": [],
            "compatible": "sprd,sc9820e",
            "display_name": "Unisoc SC9820E",
        },
    }
    runtime["assets"] = {
        "fdl1": "assets/fdl1.bin",
        "sml": "assets/sml.bin",
        "fdl2": "assets/fdl2.bin",
    }
    for role, contents in (("sml", b"secure monitor"), ("fdl2", b"board initializer")):
        (root / runtime["assets"][role]).write_bytes(contents)
    runtime["host_tools"] = {"loader": runtime["host_tools"]["loader"]}
    runtime["device_data"] = None
    runtime["nand"] = None
    runtime["sha256"]["platforms/sc9820e/host/adapter.py"] = runtime["sha256"].pop(
        "platforms/ums9117/host/adapter.py"
    )
    (root / "platforms/sc9820e/host").mkdir(parents=True)
    shutil.copyfile(
        root / "platforms/ums9117/host/adapter.py",
        root / "platforms/sc9820e/host/adapter.py",
    )
    for relative in runtime["assets"].values():
        runtime["sha256"][relative] = ""
    refresh_archive(root, runtime)
    return archive


def test_sc9820e_first_run_boots_and_same_launcher_reconnects_without_nand_preparation(
    sc9820e_archive: AutonomousArchive,
) -> None:
    """Real archive orchestration uses a controlled USB/SSH boundary without private inputs."""
    archive = sc9820e_archive
    first = archive.run()
    assert first.returncode == 0, first.stderr
    assert "Xiaomi Qin 1s+" in first.stdout
    assert "Starting FPLinux..." in first.stdout
    assert "Opening SSH." in first.stdout
    loaded = archive.data / "loaded"
    assert (loaded / "ram-loads").read_text() == "loaded\n"
    assert (loaded / "shell-opened").read_text() == "opened\n"
    assert not list(loaded.glob("*.cpio"))
    assert not (loaded / "reader-commands").exists()
    assert not archive.backup.exists()
    assert not archive.phone.exists()
    assert (archive.data / "session/session.json").is_file()
    assert "Preparing your phone data" not in first.stdout

    second = archive.run()
    assert second.returncode == 0, second.stderr
    assert "Connecting to the running phone..." in second.stdout
    assert (loaded / "ram-loads").read_text() == "loaded\n"
    assert (loaded / "shell-opened").read_text() == "opened\nopened\n"
    assert len(list(archive.logs.rglob("*.log"))) == 2


@pytest.mark.parametrize("boundary", ["loader", "shell"])
def test_sc9820e_launcher_lock_covers_loading_and_ssh(
    sc9820e_archive: AutonomousArchive, boundary: str
) -> None:
    archive = sc9820e_archive
    hold = archive.root / f"phone-hold-{boundary}"
    hold.touch()
    marker = (
        archive.data / "loaded" / ("loader-waiting" if boundary == "loader" else "shell-opened")
    )
    with archive.running() as first:
        _wait_for_file(marker, first)
        refused = archive.run()
        assert refused.returncode == 1
        assert "Another launcher is already open." in refused.stderr
        hold.unlink()
        stdout, stderr = first.communicate(timeout=30)
        assert first.returncode == 0, (stdout, stderr)
    reopened = archive.run()
    assert reopened.returncode == 0, reopened.stderr
    assert (archive.data / "loaded/ram-loads").read_text() == "loaded\n"


def test_sc9820e_reconnect_identity_failure_does_not_start_another_load(
    sc9820e_archive: AutonomousArchive,
) -> None:
    archive = sc9820e_archive
    first = archive.run()
    assert first.returncode == 0, first.stderr
    (archive.root / "phone-reconnect-failure").write_text(
        "SSH endpoint proved a different RAM session identity", encoding="utf-8"
    )
    stopped = archive.run()
    assert stopped.returncode == 1
    assert "different RAM session identity" in stopped.stderr
    assert "Starting FPLinux" not in stopped.stdout
    assert (archive.data / "loaded/ram-loads").read_text() == "loaded\n"


def test_direct_boot_archive_rejects_preparation_arguments(
    sc9820e_archive: AutonomousArchive,
) -> None:
    result = sc9820e_archive.run("--from-dump", str(sc9820e_archive.prepared.dump))
    assert result.returncode == 1
    assert "does not require phone-data preparation" in result.stderr
    assert not sc9820e_archive.phone.exists()
