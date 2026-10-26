# SPDX-License-Identifier: GPL-2.0-only
"""Shared runtime diagnostics and authenticated operations with controlled phone endpoints."""

from __future__ import annotations

import hashlib
import json
import os
import shlex
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.cli.logs import read_run
from fplinux_cli.device_data import prepare as device_data_prepare
from fplinux_cli.runtime import boot, windows_host, workflow
from fplinux_cli.runtime import bundle as bundle_runtime
from fplinux_cli.runtime.bundle import ValidatedBundle
from fplinux_cli.runtime.events import RunLog
from fplinux_cli.runtime.state import exclusive_lock, private_directory

from tests.fixtures.portable_archive import capsule_files
from tests.windows_host_support import StubWindowsHost, archive_files

if TYPE_CHECKING:
    from _hashlib import HASH
    from collections.abc import Callable, Iterator
    from io import BufferedReader
    from typing import BinaryIO


@pytest.fixture
def saved_backup_workspace(tmp_path: Path) -> Iterator[Path]:
    """Own large imported images outside the quality runtime's memory-backed temporary root."""
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    parent = Path(log_root) if log_root else tmp_path
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=parent, prefix="saved-backup-") as temporary:
        yield Path(temporary)


def test_unknown_nand_identify_reports_feature_bytes_without_reading_raw_pages(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """Identification preserves the kernel's unknown-chip report for contributor inspection."""
    report = (
        "id_bytes=deadbeef\nchip=UNKNOWN\npage_main_bytes=0\noob_bytes=0\n"
        "pages_per_block=0\nblock_count=0\nraw_bytes=0\ngeometry_source=unknown\n"
        "feature_a0=0x000000f0\nfeature_b0=0x00000000\nfeature_c0=0x00000040\n"
    )
    commands: list[str] = []

    class Phone:
        def stream_remote(
            self,
            _session: dict[str, Any],
            command: str,
            destination: BinaryIO,
            *,
            timeout: float,
            progress: Callable[[int], None] | None = None,
        ) -> None:
            del timeout, progress
            commands.append(command)
            destination.write(report.encode())

    monkeypatch.setattr(boot, "reconnect_live_bundle", lambda *_args, **_kwargs: (Phone(), {}))
    logs = tmp_path / "logs"
    logs.mkdir()
    log = RunLog(logs, protect=lambda _path: None)
    try:
        workflow._run_nand(  # noqa: SLF001 -- shared explicit NAND operation boundary.
            ValidatedBundle(
                tmp_path,
                {"target": "phone", "device_data": None, "nand": {"raw_device": "/dev/raw-nand"}},
                "a" * 64,
                "b" * 64,
                None,
            ),
            tmp_path,
            log,
            action="identify",
            output=None,
            phone_data=None,
        )
    finally:
        log.close("success")

    assert capsys.readouterr().out == report
    assert commands == ["cat /sys/class/misc/raw-nand/device/geometry 3</dev/raw-nand"]


def test_remote_command_can_use_a_live_shell_session_and_journals_its_own_result(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Explicit authenticated actions coexist with the instance lock held by a terminal."""
    data = private_directory(tmp_path / "data")
    commands = []

    class Phone:
        def run_remote(
            self, _session: dict[str, Any], command: str
        ) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            return subprocess.CompletedProcess(command, 0)

    runtime: dict[str, Any] = {
        "identity": {"target": {"display_name": "Test phone"}},
        "device_data": None,
    }
    admitted = ValidatedBundle(tmp_path, runtime, "a" * 64, "b" * 64, None)
    monkeypatch.setattr(workflow, "admit_bundle", lambda _bundle: admitted)
    monkeypatch.setattr(boot, "reconnect_live_bundle", lambda *_args, **_kwargs: (Phone(), {}))
    runtime.update(target="phone", profile=None, build_type="release")

    with exclusive_lock(data / ".launcher.lock", message="launcher occupied"):
        result = workflow.run_phone(
            tmp_path, data, tmp_path / "logs", reconnect=True, exec_command="printf ready"
        )

    assert result == 0
    assert commands == ["printf ready"]
    run = read_run(next((tmp_path / "logs").iterdir()))
    assert run is not None
    assert run.status == "success"
    assert len(run.stages) == 1
    assert run.stages[0].name == "runtime"
    assert run.stages[0].path.name == "runtime.log"


def test_manual_nand_backup_preserves_user_display_and_writes_actual_reader_bytes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The explicit NAND operation uses the normal session without phone UI commands."""
    raw = b"0123456789abcdef"
    report = (
        "id_bytes=1234\nchip=TEST\npage_main_bytes=1\noob_bytes=1\n"
        "pages_per_block=4\nblock_count=2\nraw_bytes=16\ngeometry_source=table\n"
        "feature_a0=0x00000000\nfeature_b0=0x00000000\nfeature_c0=0x00000000\n"
    )
    commands: list[str] = []

    class Phone:
        def stream_remote(
            self,
            _session: dict[str, Any],
            command: str,
            destination: BinaryIO,
            *,
            timeout: float,
            progress: Callable[[int], None] | None = None,
        ) -> None:
            del timeout
            commands.append(command)
            destination.write(report.encode() if command.startswith("cat ") else raw)
            if progress is not None:
                progress(16)

    monkeypatch.setattr(boot, "reconnect_live_bundle", lambda *_args, **_kwargs: (Phone(), {}))
    logs = tmp_path / "logs"
    logs.mkdir()
    log = RunLog(logs, protect=lambda _path: None)
    output = tmp_path / "backup.bin"
    try:
        workflow._run_nand(  # noqa: SLF001 -- shared explicit NAND operation boundary.
            ValidatedBundle(
                tmp_path,
                {
                    "target": "phone",
                    "device_data": None,
                    "nand": {"raw_device": "/dev/raw-nand", "acquisition_recipe": "a" * 64},
                },
                "a" * 64,
                "b" * 64,
                None,
            ),
            tmp_path,
            log,
            action="backup",
            output=output,
            phone_data=None,
        )
    finally:
        log.close("success")

    assert output.read_bytes() == raw
    assert commands == [
        "cat /sys/class/misc/raw-nand/device/geometry 3</dev/raw-nand",
        "exec dd if=/dev/raw-nand bs=60",
    ]


@pytest.mark.parametrize("saved_input", ["canonical", "explicit"])
def test_saved_backup_reconnects_reader_before_extraction_and_preserves_failed_preparation(
    saved_backup_workspace: Path,
    monkeypatch: pytest.MonkeyPatch,
    saved_input: str,
) -> None:
    """Real orchestration resumes a saved backup while extraction and SSH are controlled."""
    tmp_path = saved_backup_workspace
    data = tmp_path / "data"
    reader = data / "reader/session"
    reader.mkdir(parents=True)
    store = tmp_path / "phone-store"
    store.mkdir()
    saved = store / "nand.bin" if saved_input == "canonical" else tmp_path / "saved.bin"
    with saved.open("wb") as writer:
        writer.truncate(138412032)
        writer.write(b"radio")
    with saved.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    saved.with_name(f"{saved.name}.json").write_text(
        json.dumps(
            {
                "id_bytes": "e521",
                "chip": "controlled chip",
                "page_main_bytes": 2048,
                "oob_bytes": 64,
                "pages_per_block": 64,
                "block_count": 1024,
                "raw_bytes": 138412032,
                "sha256": digest,
                "target": "phone",
                "acquisition_recipe": "a" * 64,
            }
        ),
        encoding="utf-8",
    )
    from_dump = None if saved_input == "canonical" else saved
    phases: list[str] = []
    extraction_phases: list[str] = []
    original_error = OSError("controlled extraction unavailable")
    checked_storage = False

    class Phone:
        def run_remote(
            self,
            _session: dict[str, Any],
            command: str,
            *,
            capture_output: bool = False,
            shared: bool = False,
        ) -> subprocess.CompletedProcess[str]:
            nonlocal checked_storage
            assert capture_output
            assert shared
            assert reader.is_dir()
            if command.startswith("/usr/libexec/fplinux/preparation "):
                arguments = shlex.split(command)
                assert arguments[-1] == "/run/fplinux-preparation-status"
                phase = arguments[2]
                phases.append(phase)
                if phase == "error":
                    message = "controlled feedback failure"
                    raise SystemExit(message)
                if phase == "ready":
                    assert checked_storage
                    assert (store / "phone").is_dir()
                return subprocess.CompletedProcess(command, 0, "", "")
            assert "FPLINUX_SWAPS" in command
            checked_storage = True
            return subprocess.CompletedProcess(
                command,
                0,
                "rootfs / rootfs rw 0 0\n\nFPLINUX_SWAPS\nFilename Type Size Used Priority\n",
                "",
            )

    phone = Phone()

    def reconnect(
        _admitted: ValidatedBundle,
        *,
        session_root: Path,
        prepared_data_sha256: str | None,
    ) -> tuple[Phone, dict[str, Any]]:
        assert session_root == reader
        assert prepared_data_sha256 is not None
        return phone, {}

    def extract(
        _spec: device_data_prepare.DeviceDataSpec,
        backup: Path,
        destination: Path,
        _tools: device_data_prepare.DeviceDataTools,
        *,
        replace: bool,
    ) -> Path:
        assert backup == store / "nand.bin"
        assert not replace
        extraction_phases.append(phases[-1])
        if len(extraction_phases) == 1:
            raise original_error
        destination.mkdir()
        return destination

    def unexpected_boot(*_args: object, **_kwargs: object) -> None:
        pytest.fail("Saved backup preparation must not boot another phone")

    runtime: dict[str, Any] = {
        "target": "phone",
        "identity": {"target": {"compatible": "test,phone"}},
        "host_tools": {},
        "nand": {"id": 0x21E5, "raw_page_bytes": 2112, "acquisition_recipe": "a" * 64},
        "emmc": None,
        "device_data": {
            "preparation_recipe": "b" * 64,
            "parser": None,
            "originals_parser": None,
            "ecc_module": None,
            "groups": {"bluetooth": [{"source": "radio.bin", "destination": "radio.bin"}]},
            "required_groups": ["bluetooth"],
            "audio_policy": {
                "speaker_vibration": False,
                "speaker_from_headphones": False,
                "headfree_headphones_only": False,
            },
        },
    }
    admitted = ValidatedBundle(tmp_path, runtime, "a" * 64, "b" * 64, None)
    monkeypatch.setattr(boot, "reconnect_live_bundle", reconnect)
    monkeypatch.setattr(boot, "run_bundle", unexpected_boot)
    monkeypatch.setattr(device_data_prepare, "prepare_from_nand", extract)
    logs = tmp_path / "logs"
    logs.mkdir()
    log = RunLog(logs, protect=lambda _path: None)
    try:
        with pytest.raises(OSError, match="controlled extraction unavailable") as failed:
            workflow._prepare_phone_data(  # noqa: SLF001 -- real saved-reader orchestration.
                admitted,
                data,
                log,
                lambda _stage: None,
                store=store,
                from_dump=from_dump,
                replace=False,
                loader_trace=False,
            )
        assert failed.value is original_error
        assert reader.is_dir()
        assert not (store / "phone").exists()
        reader_used = workflow._prepare_phone_data(  # noqa: SLF001 -- resume saved reader/source.
            admitted,
            data,
            log,
            lambda _stage: None,
            store=store,
            from_dump=from_dump,
            replace=False,
            loader_trace=False,
        )
        assert reader_used is True
    finally:
        log.close("success")

    assert extraction_phases == ["extracting", "extracting"]
    assert phases == ["waiting", "extracting", "error", "waiting", "extracting", "ready"]
    assert not (data / "reader").exists()
    with saved.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == digest


@pytest.mark.parametrize("platform_name", ["ums9117", "sc9820e"])
def test_windows_direct_boot_reuses_its_transport_capsule_identity(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    platform_name: str,
) -> None:
    """Real capsule selection and staging use controlled native ACL, boot and SSH boundaries."""
    data = tmp_path / "data"
    data.mkdir()
    logs = tmp_path / "logs"
    logs.mkdir()
    log = RunLog(logs, protect=lambda _path: None)
    runtime: dict[str, Any] = {
        "identity": {
            "target": {"display_name": "Headless phone"},
            "platform": {"name": platform_name},
        },
        "device_data": None,
        "target": "phone",
        "transport": "usb-ncm",
        "nand": {"raw_device": "/dev/raw-nand"} if platform_name == "ums9117" else None,
    }
    capsules: list[bytes] = []
    capsule_paths: list[Path] = []
    boot_hashes: list[str | None] = []
    reconnect_hashes: list[str | None] = []
    commands: list[str] = []

    class WindowsAcl:
        """Replace unavailable native ACL calls while retaining real temporary directories."""

        def runtime_root(self, path: Path) -> Path:
            """Keep native permission preparation outside the capsule-selection scenario."""
            assert path.is_dir()
            return path

    class Phone:
        """Return only the explicit identification report through the real NAND reader."""

        def run_remote(
            self,
            _session: dict[str, Any],
            command: str,
            *,
            capture_output: bool = False,
            shared: bool = False,
        ) -> subprocess.CompletedProcess[str]:
            """Answer the shared fresh-session clock policy through this transport."""
            assert capture_output
            assert shared
            assert command.startswith("fplinux-clock ")
            return subprocess.CompletedProcess(command, 0, "system=1234 rtc=kept\n", "")

        def stream_remote(
            self,
            _session: dict[str, Any],
            command: str,
            destination: BinaryIO,
            *,
            timeout: float,
            progress: Callable[[int], None] | None = None,
        ) -> None:
            """Expose the command sent to the controlled authenticated endpoint."""
            del timeout, progress
            commands.append(command)
            destination.write(b"chip=UNKNOWN\ngeometry_source=unknown\n")

    phone = Phone()
    admitted = ValidatedBundle(tmp_path, runtime, "a" * 64, "b" * 64, None)

    def start_phone(
        _admitted: ValidatedBundle,
        _emit: Callable[[str], None],
        *,
        device_data: Path | None,
        prepared_data_sha256: str | None,
        **_options: object,
    ) -> tuple[Phone, dict[str, Any]]:
        assert device_data is not None
        capsule_paths.append(device_data)
        capsules.append(device_data.read_bytes())
        boot_hashes.append(prepared_data_sha256)
        return phone, {"status": "ready"}

    def live_phone(
        _admitted: ValidatedBundle,
        *,
        session_root: Path,
        prepared_data_sha256: str | None,
        required: bool = False,
    ) -> tuple[Phone, dict[str, Any]] | None:
        del required
        assert session_root == data / "session"
        reconnect_hashes.append(prepared_data_sha256)
        return (phone, {"status": "ready"}) if capsules else None

    monkeypatch.setattr(workflow, "os", SimpleNamespace(name="nt", isatty=lambda _fd: False))
    monkeypatch.setattr(windows_host, "WindowsHost", WindowsAcl)
    monkeypatch.setattr(boot, "run_bundle", start_phone)
    monkeypatch.setattr(boot, "reconnect_live_bundle", live_phone)
    try:
        for _launch in range(2):
            workflow._run_phone(  # noqa: SLF001 -- real shared launch selection and lifetime.
                admitted,
                data,
                log,
                lambda _stage: None,
                phone_data=None,
                from_dump=None,
                prepare_only=False,
                replace=False,
                reconnect=False,
                exec_command=None,
                upload=None,
                pull=None,
                loader_trace=False,
            )
        assert commands == []
        assert not (data / "nand.bin").exists()
        assert not (data / "phone").exists()
        assert not (data / "reader").exists()
        assert not list(data.glob(".boot-*"))
        assert len(capsules) == 1
        assert capsule_files(capsules[0]) == {"etc/fplinux/usb-transport": b"rndis\n"}
        assert not capsule_paths[0].exists()
        expected_identity = hashlib.sha256(capsules[0]).hexdigest()
        assert boot_hashes == [expected_identity]
        assert workflow.prepared_content_identity(tmp_path, runtime) == expected_identity

        assert reconnect_hashes == [expected_identity, expected_identity]
        if platform_name == "ums9117":
            workflow._run_nand(  # noqa: SLF001 -- same identity on the explicit NAND consumer.
                admitted,
                data,
                log,
                action="identify",
                output=None,
                phone_data=None,
            )
            assert reconnect_hashes == [expected_identity, expected_identity, expected_identity]
            assert commands == ["cat /sys/class/misc/raw-nand/device/geometry 3</dev/raw-nand"]
    finally:
        log.close("success")


def test_linux_qin_content_identity_does_not_add_a_transport_capsule(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A direct SC9820E boot remains free of supplemental UMS transport data."""
    monkeypatch.setattr(workflow, "os", SimpleNamespace(name="posix"))
    runtime = {"device_data": None, "identity": {"platform": {"name": "sc9820e"}}}

    assert workflow.prepared_content_identity(tmp_path, runtime) is None


@pytest.mark.parametrize("host", ["posix", "nt"])
def test_bundle_admission_hashes_the_image_once_and_reconnect_reuses_its_context(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    host: str,
) -> None:
    """Real image admission is reused across absent-session discovery on both host branches."""
    root = archive_files(tmp_path / "archive")
    if host == "posix":
        (root / "windows-host.json").unlink()
        runtime = json.loads((root / "runtime-manifest.json").read_text())
        for relative in runtime["host_tools"].values():
            (root / relative).chmod(0o755)
    image = root / "image/ramboot.bin"
    image_reads: list[Path] = []
    original_digest = hashlib.file_digest

    def measured_digest(source: BufferedReader, algorithm: str) -> HASH:
        if Path(source.name) == image:
            image_reads.append(image)
        return original_digest(source, algorithm)

    monkeypatch.setattr(hashlib, "file_digest", measured_digest)
    monkeypatch.setattr(
        bundle_runtime, "os", SimpleNamespace(name=host, access=os.access, X_OK=os.X_OK)
    )
    admitted = bundle_runtime.admit_bundle(root)
    assert image_reads == [image]
    monkeypatch.setattr(boot, "os", SimpleNamespace(name=host))
    if host == "nt":
        monkeypatch.setattr(windows_host, "WindowsHost", lambda: StubWindowsHost({}))

    assert boot.reconnect_live_bundle(admitted, session_root=tmp_path / "session") is None
    assert image_reads == [image]
