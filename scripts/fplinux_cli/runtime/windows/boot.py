# SPDX-License-Identifier: GPL-2.0-only
"""Windows USB loader and boot lifecycle."""

from __future__ import annotations

import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .host import BootromDriverRequiredError, WindowsHost, exact_usb_devices, fail
from .session import prepare_session
from .transport import WindowsTransport

if TYPE_CHECKING:
    from collections.abc import Callable

    from .bundle import WindowsBundle


def _setup_bootrom_driver(
    host: WindowsHost,
    bundle: WindowsBundle,
    directory: Path,
    *,
    events: Callable[[str], None] | None,
    log: Path | None = None,
) -> None:
    """Install after native driver refusal, then await a fresh physical connection."""
    usb = bundle.runtime["usb"]["bootrom"]
    if (usb["vendor_id"], usb["product_id"]) != (0x1782, 0x4D00):
        fail("Automatic USB driver setup is unavailable for this BootROM USB ID")
    try:
        accepted = host.confirm_driver_install()
    except subprocess.TimeoutExpired:
        fail("USB driver consent timed out; disconnect the phone and restart Run.cmd")
    if not accepted:
        fail("USB driver setup was declined; disconnect the phone and restart Run.cmd")
    if events is not None:
        events("driver-setup")
    host.install_bootrom_driver(bundle.tools["driver"], directory, log=log)
    if events is not None:
        events("driver-reconnect")
    deadline = time.monotonic() + usb["wait_seconds"]
    while exact_usb_devices(host.ps("devices.ps1"), usb["vendor_id"], usb["product_id"]):
        if time.monotonic() >= deadline:
            fail("Disconnect the phone after USB driver setup, then restart Run.cmd")
        time.sleep(0.2)


def boot_archive(  # noqa: PLR0913 -- native loading inputs have separate owners.
    bundle: WindowsBundle,
    device_data: Path,
    *,
    events: Callable[[str], None] | None = None,
    log: Path,
    session_root: Path,
    prepared_data_sha256: str | None = None,
    loader_trace: bool = False,
    on_wait: Callable[[], None] | None = None,
) -> tuple[WindowsTransport, dict[str, Any]]:
    """Boot one selected archive with its external data capsule and return authenticated SSH."""
    if events is not None:
        events("host-preparation")
    root = bundle.root
    if device_data.is_symlink() or not device_data.is_file():
        fail("The prepared device data capsule is missing")
    host = WindowsHost()
    usb = bundle.runtime["usb"]["bootrom"]
    if exact_usb_devices(host.ps("devices.ps1"), usb["vendor_id"], usb["product_id"]):
        fail(
            "Disconnect the powered-off phone and restart Run.cmd; "
            "connect only after its invitation"
        )
    adapter = bundle.adapter
    config = adapter.adapter_config(bundle.runtime["adapter"])
    log.parent.mkdir(parents=True, exist_ok=True)
    session = prepare_session(
        host,
        bundle,
        events=events,
        session_root=session_root,
        prepared_data_sha256=prepared_data_sha256,
    )
    directory = Path(session["directory"])
    image = Path(session["image"])
    split_upload = bundle.runtime["identity"]["platform"]["name"] == "sc9820e"
    ready = False
    try:
        loader_directory = directory
        if split_upload:
            loader_directory = directory / "uploads"
            loader_directory.mkdir()
            host.secure_new(loader_directory, directory=True)
            uploads = adapter.prepare_uploads(
                root, bundle.runtime, session, loader_directory, device_data=device_data
            )
            loader_arguments = adapter.loader_arguments(
                bundle.tools["loader"],
                uploads,
                config,
                bus=None,
                device=None,
                wait_seconds=usb["wait_seconds"],
                working_directory=loader_directory,
            )
        else:
            shutil.copyfile(root / bundle.runtime["assets"]["fdl1"], directory / "fdl1.bin")
            loader_arguments = adapter.loader_arguments(
                bundle.tools["loader"],
                Path("fdl1.bin"),
                Path("image.bin"),
                bundle.runtime["addresses"],
                config["exec_distance"],
                wait_seconds=usb["wait_seconds"],
            )
        driver_installed = False

        def usb_ready() -> None:
            if events is not None:
                events("device-found")
                if driver_installed:
                    events("driver-ready")
                events("ram-loader-start")

        while True:
            print(
                f"{bundle.runtime['identity']['target']['display_name']} console RAM boot",
                flush=True,
            )
            print(config["boot_instructions"], flush=True)
            if events is not None:
                events("waiting-for-device")
            try:
                host.run_tool(
                    [loader_arguments[0], "--verbose", "1", *loader_arguments[1:]]
                    if loader_trace
                    else loader_arguments,
                    timeout=300 + usb["wait_seconds"],
                    cwd=loader_directory,
                    log=log,
                    environment={"LIBUSB_DEBUG": "4"} if loader_trace else None,
                    on_usb_ready=usb_ready,
                    on_wait=on_wait,
                )
            except BootromDriverRequiredError:
                if driver_installed:
                    fail("WinUSB is still unavailable after setup; disconnect and restart Run.cmd")
                _setup_bootrom_driver(host, bundle, directory, events=events, log=log)
                driver_installed = True
            else:
                break
        if events is not None:
            events("ram-loader-complete")
        image.unlink()
        if split_upload:
            shutil.rmtree(loader_directory)
        else:
            (directory / "fdl1.bin").unlink()
            payload = directory / "payload"
            payload.mkdir()
            host.secure_new(payload, directory=True)
            sources = adapter.loader_payload_sources(root, device_data=device_data)
            for name, source in sources.items():
                destination = payload / name
                shutil.copyfile(source, destination)
                host.secure_new(destination)
        if not split_upload:
            if events is not None:
                events("bootstrap-handoff")
            host.run_tool(
                [
                    str(bundle.tools["bridge"]),
                    "--wait",
                    str(config["handoff_wait_seconds"]),
                    "--fplinux-handoff",
                    session["session_id"],
                    "--",
                    *adapter.fpdoom_options(config),
                    config["session_name"],
                ],
                timeout=config["handoff_wait_seconds"],
                cwd=payload,
                log=log,
                environment={"LIBUSB_DEBUG": "4"} if loader_trace else None,
            )
        if events is not None:
            events("linux-start")
        print("Waiting for the authenticated Linux connection...", flush=True)
        deadline = time.monotonic() + config["usb_release_wait_seconds"]
        while exact_usb_devices(host.ps("devices.ps1"), usb["vendor_id"], usb["product_id"]):
            if time.monotonic() >= deadline:
                fail("The BootROM USB device did not release after the Linux transition")
            time.sleep(0.2)
        if events is not None:
            events("linux-transition")
        transport = WindowsTransport(host, bundle)
        transport.authenticate(
            session, timeout=bundle.runtime["usb"]["linux_gadget"]["wait_seconds"]
        )
        ready = True
        transport.retire_previous_sessions(session)
        if events is not None:
            events("ssh-ready")
        return transport, session
    finally:
        original_error = sys.exception()
        try:
            for temporary in (image, directory / "fdl1.bin"):
                temporary.unlink(missing_ok=True)
            for stage in (directory / "payload", directory / "uploads", directory / "driver"):
                if stage.exists():
                    shutil.rmtree(stage)
            if not ready:
                shutil.rmtree(directory)
        except OSError:
            if original_error is None:
                raise
