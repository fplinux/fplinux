#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-2.0-only
"""Start this phone's Linux system or open its existing SSH connection."""

from __future__ import annotations

import argparse
import errno
import importlib
import os
import subprocess
import sys
import tempfile
import time
import traceback
from contextlib import contextmanager, redirect_stderr, redirect_stdout
from datetime import UTC, datetime
from pathlib import Path
from typing import TYPE_CHECKING, Any

ROOT = Path(__file__).resolve().parent
sys.path[:0] = [str(ROOT / "scripts"), str(ROOT)]

from fplinux_cli.common import fail, read_json_object, replace_file_atomically  # noqa: E402
from fplinux_cli.device_data.cpio import create_device_data_initramfs  # noqa: E402
from fplinux_cli.device_data.inputs import capture_device_data_generation  # noqa: E402
from fplinux_cli.device_data.portable import prepare_portable_device_data  # noqa: E402
from fplinux_cli.manifests.audio import validate_audio_policy  # noqa: E402
from fplinux_cli.manifests.identity import validate_compatible  # noqa: E402
from fplinux_cli.manifests.values import basename_value, exact_table  # noqa: E402
from fplinux_cli.runtime.nand_backup import backup_nand  # noqa: E402

if TYPE_CHECKING:
    from collections.abc import Iterator
    from typing import TextIO

    from fplinux_cli.device_data.inputs import FirmwareInput


def _private_directory(path: Path) -> Path:
    """Protect local backups and connection keys on either supported host."""
    created = not path.exists()
    path.mkdir(mode=0o700, exist_ok=True)
    if path.is_symlink() or not path.is_dir():
        fail(f"Private data directory is invalid: {path}")
    if os.name == "nt":
        from common.windows_host import WindowsHost  # noqa: PLC0415

        host = WindowsHost()
        if created:
            host.secure_new(path, directory=True)
        else:
            host.require_private(path)
    else:
        path.chmod(0o700)
    return path


@contextmanager
def _launcher_lock(root: Path) -> Iterator[None]:
    """Keep one launch active until preparation and its interactive SSH client finish."""
    descriptor = os.open(root / ".launcher.lock", os.O_RDWR | os.O_CREAT, 0o600)
    try:
        try:
            if sys.platform == "win32":
                import msvcrt  # noqa: PLC0415 -- only available on Windows.

                msvcrt.locking(descriptor, msvcrt.LK_NBLCK, 1)
            else:
                import fcntl  # noqa: PLC0415 -- only available on Linux.

                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno not in {errno.EACCES, errno.EAGAIN, errno.EDEADLK}:
                raise
            fail("This archive is already running. Close its existing launcher first.")
        yield
    finally:
        # Keep the file: unlinking it could let another launch lock a different inode.
        os.close(descriptor)


class RunLog:
    """One launch's diagnostics, separate from interactive SSH input and output."""

    def __init__(self, root: Path) -> None:
        """Create a private log for this launch and keep its terminal stream."""
        self.started = time.monotonic()
        directory = _private_directory(root / "logs")
        stamp = datetime.now(UTC).strftime("%Y-%m-%d_%H%M%S_%f")
        self.path = directory / f"{stamp}.log"
        descriptor = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_APPEND, 0o600)
        self.stream = os.fdopen(descriptor, "w", encoding="utf-8", buffering=1)
        self.visible = sys.stdout
        self.stage_started = self.started
        self.stage = ""

    def message(self, text: str) -> None:
        """Show one status message and retain its timestamp."""
        print(text, file=self.visible, flush=True)
        print(f"{datetime.now(UTC).isoformat()} {text}", file=self.stream, flush=True)

    def event(self, stage: str, runtime: dict[str, Any]) -> None:
        """Show loader stages once and record elapsed stage time."""
        now = time.monotonic()
        if self.stage:
            print(f"Stage {self.stage}: {now - self.stage_started:.2f}s", file=self.stream)
        self.stage, self.stage_started = stage, now
        messages = {
            "host-preparation": "Checking the computer and USB access...",
            "network-preparation": "Choosing the private USB connection...",
            "session-preparation": "Creating secure access for this boot...",
            "payload-preparation": "Preparing the RAM image...",
            "waiting-for-device": runtime["adapter"]["boot_instructions"],
            "device-found": "Phone detected...",
            "ram-loader-start": "Loading the phone into RAM; keep USB connected...",
            "ram-loader-complete": "Phone loaded into RAM.",
            "linux-start": "Starting Linux...",
            "linux-transition": "Waiting for the phone's USB network and SSH...",
            "ssh-ready": "Secure phone connection is ready.",
            "driver-setup": "Follow the Windows USB driver setup window.",
            "driver-ready": "Phone USB driver is ready.",
            "usb-access-setup": "Setting up USB access; Linux may ask for your password once.",
        }
        if stage in messages:
            self.message(messages[stage])

    @contextmanager
    def capture(self) -> Iterator[TextIO]:
        """Retain backend diagnostics while progress uses the original terminal."""
        with redirect_stdout(self.stream), redirect_stderr(self.stream):
            yield self.stream

    def close(self) -> None:
        """Finish this launch's log, including errors and cancellations."""
        print(f"Launch ended after {time.monotonic() - self.started:.2f}s", file=self.stream)
        self.stream.close()


def _archive_runtime(root: Path) -> dict[str, Any]:
    windows_archive = (root / "windows-host.json").is_file()
    if windows_archive != (os.name == "nt"):
        command = "Run.cmd on Windows" if windows_archive else "Run.sh on Linux"
        fail(f"Open this archive with {command}")
    run = importlib.import_module("runner.run")
    runtime: dict[str, Any] = run.load_runtime_manifest(root / "runtime-manifest.json")
    runtime = run.validate_runtime_identity(runtime)
    if not windows_archive:
        for relative, expected in runtime["sha256"].items():
            path = root / relative
            run.require_file(path, executable=relative in runtime["host_tools"].values())
            # The transport admits the image against the build manifest once.
            if relative != runtime["image"] and run.digest(path) != expected:
                fail(f"Archive file checksum differs: {relative}")
    return runtime


def _device_data_manifest(root: Path, runtime: dict[str, Any]) -> dict[str, Any]:
    manifest = exact_table(
        read_json_object(root / "device-data-manifest.json"),
        {"target", "platform", "machine_compatible", "audio_policy", "nand", "device_data"},
        "archive device-data manifest",
    )
    target = basename_value(manifest["target"], "archive target")
    platform = basename_value(manifest["platform"], "archive platform")
    if target != runtime["target"] or platform != runtime["identity"]["platform"]["name"]:
        fail("The device-data manifest describes a different phone than this archive")
    try:
        compatible = validate_compatible(manifest["machine_compatible"], "archive compatible")
        validate_audio_policy(manifest["audio_policy"], "archive audio policy")
    except ValueError as error:
        fail(str(error))
    if compatible != runtime["identity"]["target"]["compatible"]:
        fail("The device-data manifest describes a different phone than this archive")
    device_data = exact_table(manifest["device_data"], {"parser", "groups"}, "device_data")
    if not device_data["groups"]:
        fail("This archive does not declare personal device data")
    return manifest


def _connection(
    root: Path,
    runtime: dict[str, Any],
    log: RunLog,
    *,
    capsule: Path,
    session_root: Path | None = None,
) -> tuple[Any, dict[str, Any]]:
    with log.capture() as output:
        if os.name == "nt":
            from common.windows_host import boot_archive  # noqa: PLC0415

            return boot_archive(
                root,
                capsule,
                events=lambda stage: log.event(stage, runtime),
                log=log.path,
                session_root=session_root,
            )
        run = importlib.import_module("runner.run")
        options = argparse.Namespace(reconnect=False, exec_command=None, upload=None, pull=None)
        handoff = run.run_bundle(
            root,
            runtime,
            options,
            lambda stage: log.event(stage, runtime),
            device_data=capsule,
            output=output,
            setup_host=True,
            session_root=session_root or root / "data/session",
        )
    if handoff is None:
        fail("The phone did not establish its authenticated Linux connection")
    transport, session = handoff
    return transport, session


def _live_connection(
    root: Path, runtime: dict[str, Any], log: RunLog
) -> tuple[Any, dict[str, Any]] | None:
    with log.capture():
        if os.name == "nt":
            from common.windows_host import load_live_session  # noqa: PLC0415

            return load_live_session(root, runtime["target"])
        run = importlib.import_module("runner.run")
        connection: tuple[Any, dict[str, Any]] | None = run.reconnect_live_bundle(
            root, runtime, session_root=root / "data/session"
        )
        return connection


def _write_capsule(directory: Path, admitted: dict[str, tuple[FirmwareInput, ...]]) -> Path:
    capsule = directory / "device-data.cpio"
    for firmware in admitted.get("board-maps", ()):
        if firmware.destination in {"pinmap.bin", "keymap.bin"}:
            replace_file_atomically(directory / firmware.destination, firmware.contents, 0o600)
    contents = create_device_data_initramfs(
        admitted, usb_transport="rndis" if os.name == "nt" else None
    )
    replace_file_atomically(capsule, contents, 0o600)
    return capsule


def _selected_data(
    data: Path, manifest: dict[str, Any]
) -> dict[str, tuple[FirmwareInput, ...]] | None:
    phone = data / "phone"
    if not phone.exists():
        return None
    return capture_device_data_generation(
        manifest["device_data"]["groups"], phone, path_field="source", require_all=True
    )


def _read_backup(
    root: Path, runtime: dict[str, Any], manifest: dict[str, Any], data: Path, log: RunLog
) -> Path:
    nand = manifest["nand"]
    if not isinstance(nand, dict) or not isinstance(nand.get("raw_device"), str):
        fail("This archive does not declare its read-only NAND reader")
    log.message("First run: keep the phone powered off and disconnected.")
    started = time.monotonic()
    last_update = 0.0

    def progress(completed: int, total: int) -> None:
        nonlocal last_update
        now = time.monotonic()
        if completed not in (0, total) and now - last_update < 1:
            return
        last_update = now
        elapsed = max(now - started, 0.001)
        mib = 1024 * 1024
        text = (
            f"Reading NAND: {completed / total:6.1%} "
            f"({completed / mib:.1f}/{total / mib:.1f} MiB) "
            f"· {completed / mib / elapsed:.2f} MiB/s"
        )
        if log.visible.isatty():
            print(
                f"\r{text}", end="\n" if completed == total else "", file=log.visible, flush=True
            )
        else:
            print(text, file=log.visible, flush=True)
        print(text, file=log.stream, flush=True)

    # This reader boot must never be reused as a boot with the prepared phone data.
    with tempfile.TemporaryDirectory(prefix=".initial-boot-", dir=data) as temporary:
        directory = Path(temporary)
        connection = _connection(
            root,
            runtime,
            log,
            capsule=_write_capsule(directory, {}),
            session_root=directory / "session",
        )
        log.message("Reading the complete NAND backup; keep USB connected. Ctrl+C cancels.")
        started = time.monotonic()
        with log.capture():
            backup = backup_nand(
                lambda: connection,
                data / "nand.bin",
                target=manifest["target"],
                raw_device=nand["raw_device"],
                declared_id=nand.get("id"),
                declared_raw_page_bytes=nand.get("raw_page_bytes"),
                progress=progress,
            )
    log.message(f"Backup saved: {backup}")
    return backup


def run_archive(root: Path, log: RunLog, *, from_dump: Path | None = None) -> None:
    runtime = _archive_runtime(root)
    manifest = _device_data_manifest(root, runtime)
    data = _private_directory(root / "data")
    log.message(f"FPLinux — {runtime['identity']['target']['display_name']}")
    log.message("Internal storage is read only. Ctrl+C cancels the current operation.")
    selected = _selected_data(data, manifest)
    if from_dump is not None or selected is None:
        backup = from_dump or data / "nand.bin"
        if from_dump is None and not backup.exists():
            backup = _read_backup(root, runtime, manifest, data, log)
        log.message("Preparing your phone data...")
        with log.capture():
            phone = prepare_portable_device_data(from_dump=backup, destination=data / "phone")
            admitted = capture_device_data_generation(
                manifest["device_data"]["groups"], phone, path_field="source", require_all=True
            )
            _write_capsule(phone, admitted)
        log.message("Your phone data is ready in data/phone.")
        log.message("Disconnect USB. If the phone stays on, remove and reinsert its battery.")
        log.message("Run the same script again with the phone powered off.")
        return
    log.message("Checking the saved phone connection...")
    connection = _live_connection(root, runtime, log)
    if connection is None:
        log.message("Starting FPLinux with your saved phone data...")
        capsule = _write_capsule(data / "phone", selected)
        transport, session = _connection(root, runtime, log, capsule=capsule)
    else:
        log.message("Connecting to the running phone...")
        transport, session = connection
    log.message("Opening SSH. Type exit or press Ctrl+D to close the terminal.")
    # Interactive terminal contents and commands are not copied into the launch log.
    status = transport.open_shell(session) if os.name == "nt" else transport.run_shell(session)
    if status:
        fail(f"SSH exited with status {status}")
    log.message("SSH terminal closed. Open the same launcher to connect again.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--from-dump", type=Path, metavar="PATH", help="prepare from your saved NAND backup"
    )
    options = parser.parse_args()
    log: RunLog | None = None
    try:
        with _launcher_lock(ROOT):
            print("Preparing FPLinux...", flush=True)
            log = RunLog(ROOT)
            log.message(f"Log: {log.path}")
            run_archive(ROOT, log, from_dump=options.from_dump)
    except KeyboardInterrupt:
        if log is not None:
            log.message("Cancelled. Cancelling does not power the phone off.")
        raise SystemExit(130) from None
    except (OSError, ValueError, SystemExit, subprocess.TimeoutExpired) as error:
        if isinstance(error, SystemExit) and error.code in (129, 130, 143):
            if log is not None:
                log.message("Cancelled. Cancelling does not power the phone off.")
            raise
        if log is not None:
            traceback.print_exception(error, file=log.stream)
        detail = (
            f"Computer connection setup timed out after {error.timeout:g} seconds."
            if isinstance(error, subprocess.TimeoutExpired)
            else str(error)
        )
        for prefix in ("FPLinux: ", "fplinux: ", "fplinux run: ", "fplinux ssh: "):
            detail = detail.removeprefix(prefix)
        print(f"FPLinux: {detail}", file=sys.stderr, flush=True)
        if log is not None:
            print(f"See {log.path} for details.", file=sys.stderr, flush=True)
        raise SystemExit(1) from None
    finally:
        if log is not None:
            log.close()


if __name__ == "__main__":
    main()
