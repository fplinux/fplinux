#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare this phone on the first run and boot its personal data on later runs."""

from __future__ import annotations

import argparse
import importlib
import os
import subprocess
import sys
import traceback
from contextlib import redirect_stderr, redirect_stdout
from datetime import UTC, datetime
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast

ROOT = Path(__file__).resolve().parent
sys.path[:0] = [str(ROOT / "scripts"), str(ROOT)]

from fplinux_cli.common import fail, read_json_object, replace_file_atomically  # noqa: E402
from fplinux_cli.device_data.cpio import create_device_data_initramfs  # noqa: E402
from fplinux_cli.device_data.inputs import (  # noqa: E402
    capture_device_data_generation,
    current_device_data_generation,
)
from fplinux_cli.device_data.portable import prepare_portable_device_data  # noqa: E402
from fplinux_cli.manifests.values import basename_value, exact_table  # noqa: E402
from fplinux_cli.runtime.nand_backup import backup_nand  # noqa: E402

if TYPE_CHECKING:
    from fplinux_cli.device_data.inputs import FirmwareInput
    from fplinux_cli.runtime.nand_backup import SshTransport


def _validate_linux_archive(root: Path) -> dict[str, Any]:
    """Admit the canonical runtime and its identity before any phone operation."""
    run = importlib.import_module("runner.run")
    runtime: dict[str, Any] = run.load_runtime_manifest(root / "runtime-manifest.json")
    for relative, expected in runtime["sha256"].items():
        path = root / relative
        run.require_file(path, executable=relative in runtime["host_tools"].values())
        if run.digest(path) != expected:
            fail(f"Archive file checksum differs: {relative}")
    runtime = run.validate_runtime_identity(runtime)
    if runtime["transport"] != "usb-ncm":
        fail("This archive does not support the authenticated Linux connection")
    ssh = run.load_module(root / run.SSH_HELPER_PATH, "ssh_transport")
    ssh.bundle_identity(root, runtime)
    return runtime


def _archive_runtime(root: Path) -> dict[str, Any]:
    windows_archive = (root / "windows-host.json").is_file()
    if windows_archive != (os.name == "nt"):
        command = "Run.cmd on Windows" if windows_archive else "Run.sh on Linux"
        fail(f"Open this archive with {command}")
    if windows_archive:
        from common.windows_host import validate_archive  # noqa: PLC0415

        return validate_archive(root).runtime
    return _validate_linux_archive(root)


def _device_data_manifest(root: Path, runtime: dict[str, Any]) -> dict[str, Any]:
    manifest = exact_table(
        read_json_object(root / "device-data-manifest.json"),
        {"target", "platform", "nand", "device_data"},
        "archive device-data manifest",
    )
    target = basename_value(manifest["target"], "archive target")
    platform = basename_value(manifest["platform"], "archive platform")
    if target != runtime["target"] or platform != runtime["identity"]["platform"]["name"]:
        fail("The device-data manifest describes a different phone than this archive")
    device_data = exact_table(manifest["device_data"], {"parser", "groups"}, "device_data")
    if not device_data["groups"]:
        fail("This archive does not declare personal device data")
    return manifest


def _boot_archive(
    root: Path, runtime: dict[str, Any], capsule: Path
) -> tuple[SshTransport, dict[str, Any]]:
    visible = sys.stdout

    def progress(stage: str) -> None:
        print(f"Stage: {stage}", flush=True)
        if stage == "host-preparation":
            print(
                "Preparing the Windows host and checking USB devices...", file=visible, flush=True
            )
        elif stage == "network-preparation":
            print("Preparing the private USB connection...", file=visible, flush=True)
        elif stage == "session-preparation":
            print("Preparing secure access to the phone...", file=visible, flush=True)
        elif stage == "payload-preparation":
            print("Preparing the RAM image...", file=visible, flush=True)
        elif stage == "waiting-for-device":
            print(runtime["adapter"]["boot_instructions"], file=visible, flush=True)
        elif stage == "device-found":
            print("Phone detected. Checking the USB driver...", file=visible, flush=True)
        elif stage == "ram-loader-start":
            print(
                "Loading the phone into RAM. Keep the USB cable connected...",
                file=visible,
                flush=True,
            )
        elif stage == "ram-loader-complete":
            print("Phone loaded into RAM.", file=visible, flush=True)
        elif stage == "linux-start":
            print("Starting Linux on the phone...", file=visible, flush=True)
        elif stage == "linux-transition":
            print("Waiting for the Linux connection...", file=visible, flush=True)
        elif stage == "ssh-ready":
            print("Secure phone connection is ready.", file=visible, flush=True)
        elif stage == "driver-setup":
            print(
                "Windows needs the phone USB driver. Follow the setup window.",
                file=visible,
                flush=True,
            )
        elif stage == "driver-ready":
            print("Phone USB driver is ready.", file=visible, flush=True)
        elif stage == "usb-access-setup":
            print(
                "Setting up phone USB access. Linux may ask for your password once.",
                file=visible,
                flush=True,
            )

    with (
        (root / ".cache/run.log").open("a", encoding="utf-8", buffering=1) as output,
        redirect_stdout(output),
        redirect_stderr(output),
    ):
        print(f"\nRAM boot started: {datetime.now(UTC).isoformat()}", flush=True)
        if os.name == "nt":
            from common.windows_host import boot_archive  # noqa: PLC0415

            return boot_archive(root, capsule, events=progress)
        run = importlib.import_module("runner.run")
        options = argparse.Namespace(reconnect=False, exec_command=None, upload=None, pull=None)
        handoff = run.run_bundle(
            root, runtime, options, progress, device_data=capsule, output=output, setup_host=True
        )
    if handoff is None:
        fail("The phone did not establish its authenticated Linux connection")
    ssh, session = handoff
    return cast("SshTransport", ssh), session


def _write_capsule(
    directory: Path,
    admitted: dict[str, tuple[FirmwareInput, ...]],
) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    capsule = directory / "device-data.cpio"
    contents = create_device_data_initramfs(
        admitted, usb_transport="rndis" if os.name == "nt" else None
    )
    for firmware in admitted.get("board-maps", ()):
        if firmware.destination in {"pinmap.bin", "keymap.bin"}:
            replace_file_atomically(directory / firmware.destination, firmware.contents, 0o600)
    replace_file_atomically(capsule, contents, 0o600)
    return capsule


def _selected_data(
    root: Path, manifest: dict[str, Any]
) -> tuple[Path, dict[str, tuple[FirmwareInput, ...]]] | None:
    generation = current_device_data_generation(root / ".cache", manifest["target"])
    if generation is None:
        return None
    declarations = manifest["device_data"]["groups"]
    admitted = capture_device_data_generation(
        declarations, generation, path_field="source", require_all=False
    )
    if set(admitted) != set(declarations):
        return None
    return generation, admitted


def _prepare_cache(root: Path) -> Path:
    """Keep full phone backups and prepared data private on either host."""
    cache = root / ".cache"
    created = not cache.exists()
    cache.mkdir(mode=0o700, exist_ok=True)
    if os.name == "nt":
        from common.windows_host import WindowsHost  # noqa: PLC0415

        host = WindowsHost()
        if created:
            host.secure_new(cache, directory=True)
        else:
            host.require_private(cache)
    else:
        cache.chmod(0o700)
    return cache


def run_archive(root: Path, *, from_dump: Path | None = None) -> None:
    """Keep preparation and subsequent personal-data boots as separate user actions."""
    runtime = _archive_runtime(root)
    manifest = _device_data_manifest(root, runtime)
    cache = _prepare_cache(root)
    print(f"FPLinux — {runtime['identity']['target']['display_name']}", flush=True)
    print("Internal storage is read only.", flush=True)
    selected = _selected_data(root, manifest)
    if from_dump is None and selected is not None:
        generation, admitted = selected
        print("Your phone data is ready. Starting FPLinux...", flush=True)
        capsule = _write_capsule(generation, admitted)
        _boot_archive(root, runtime, capsule)
        print("FPLinux is ready on the phone. Use its screen and keys.", flush=True)
        return

    log = cache / "run.log"
    if from_dump is None:
        print("First run: save your phone's internal storage and prepare its data.", flush=True)
        print("Keep the phone disconnected until the connection instructions appear.", flush=True)
        nand = manifest["nand"]
        if not isinstance(nand, dict) or not isinstance(nand.get("raw_device"), str):
            fail("This archive does not declare its read-only NAND reader")
        capsule = _write_capsule(cache / "initial-boot", {})
        connection = _boot_archive(root, runtime, capsule)
        print("Saving a complete backup. Keep the USB cable connected...", flush=True)
        with log.open("a", encoding="utf-8") as output, redirect_stdout(output):
            from_dump = backup_nand(
                lambda: connection,
                cache / "nand.bin",
                target=manifest["target"],
                raw_device=nand["raw_device"],
                declared_id=nand.get("id"),
                declared_raw_page_bytes=nand.get("raw_page_bytes"),
            )
        print("Backup saved in .cache/nand.bin.", flush=True)

    print("Preparing your phone data...", flush=True)
    with log.open("a", encoding="utf-8") as output, redirect_stdout(output):
        generation = prepare_portable_device_data(from_dump=from_dump)
        admitted = capture_device_data_generation(
            manifest["device_data"]["groups"],
            generation,
            path_field="source",
            require_all=True,
        )
        _write_capsule(generation, admitted)
    print("Your phone data is ready.", flush=True)
    print(
        "Disconnect USB. If the phone stays on, briefly remove and reinsert its battery.",
        flush=True,
    )
    print("Run this same script again with the phone powered off.", flush=True)


def main() -> None:
    """Expose a fixed-phone entry point and optional existing-backup preparation."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--from-dump",
        type=Path,
        metavar="PATH",
        help="prepare this phone's data from an existing complete NAND backup",
    )
    options = parser.parse_args()
    try:
        run_archive(ROOT, from_dump=options.from_dump)
    except (OSError, ValueError, SystemExit, subprocess.TimeoutExpired) as error:
        log = ROOT / ".cache/run.log"
        log.parent.mkdir(exist_ok=True)
        with log.open("a", encoding="utf-8") as output:
            traceback.print_exception(error, file=output)
        detail = (
            f"Computer connection setup timed out after {error.timeout:g} seconds."
            if isinstance(error, subprocess.TimeoutExpired)
            else str(error).removeprefix("FPLinux: ")
        )
        print(f"FPLinux: {detail}\nSee .cache/run.log for details.", file=sys.stderr, flush=True)
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
