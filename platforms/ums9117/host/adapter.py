# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Fixed UMS9117 host translation for the shared RAM runner."""

from __future__ import annotations

import importlib
import os
import re
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any, NoReturn

BACKLIGHT_CHANNELS = "rgbw"
SESSION_ID = re.compile(r"[0-9a-f]{64}\Z")


def fail(message: str) -> NoReturn:
    raise SystemExit(f"RAM adapter failed: {message}")


class BootromDisconnectedError(Exception):
    """The enumerated BootROM node vanished before userspace opened it."""


def integer(
    table: dict[str, Any],
    key: str,
    *,
    bounds: tuple[int, int],
    allowed: set[int] | None = None,
) -> int:
    value = table.get(key)
    minimum, maximum = bounds
    if type(value) is not int or not minimum <= value <= maximum:
        fail(f"adapter {key} must be an integer in {minimum}..{maximum}")
    if allowed is not None and value not in allowed:
        fail(f"adapter {key} must be one of {', '.join(str(item) for item in sorted(allowed))}")
    return value


def text(table: dict[str, Any], key: str) -> str:
    value = table.get(key)
    if not isinstance(value, str) or not value:
        fail(f"adapter {key} must be a non-empty string")
    return value


def backlight_channels(table: dict[str, Any], key: str) -> str:
    """Return a non-empty, canonically ordered subset of RGBW channels."""
    value = text(table, key)
    canonical = "".join(channel for channel in BACKLIGHT_CHANNELS if channel in value)
    if value != canonical:
        fail(f"adapter {key} must be a canonical non-empty subset of {BACKLIGHT_CHANNELS}")
    return value


def prepared_session_token(value: object) -> str:
    """Return the exact session token that binds bridge completion to this run."""
    if not isinstance(value, dict):
        fail("RAM loader requires a prepared session")
    token = value.get("session_id")
    if not isinstance(token, str) or SESSION_ID.fullmatch(token) is None:
        fail("prepared session has an invalid session_id")
    return token


def runtime_target_display_name(runtime: dict[str, Any]) -> str:
    """Return the target name only for the exact UMS9117 platform contract."""
    identity = runtime.get("identity")
    if not isinstance(identity, dict):
        fail("runtime identity must be an object")
    platform = identity.get("platform")
    if not isinstance(platform, dict) or platform.get("name") != "ums9117":
        fail("runtime platform identity must name ums9117")
    target = identity.get("target")
    if not isinstance(target, dict):
        fail("runtime target identity must be an object")
    display_name = target.get("display_name")
    if not isinstance(display_name, str) or not display_name:
        fail("runtime target display_name must be a non-empty string")
    return display_name


def adapter_config(value: object) -> dict[str, Any]:
    keys = {
        "brightness",
        "rotation",
        "spi_mode",
        "lcd_id",
        "exec_distance",
        "backlight_channels",
        "backlight_level",
        "session_name",
        "handoff_wait_seconds",
        "usb_release_wait_seconds",
        "boot_instructions",
    }
    if not isinstance(value, dict) or set(value) != keys:
        fail(f"adapter data must contain exactly: {', '.join(sorted(keys))}")
    return {
        "brightness": integer(value, "brightness", bounds=(0, 100)),
        "rotation": integer(
            value,
            "rotation",
            bounds=(0, 270),
            allowed={0, 90, 180, 270},
        ),
        "spi_mode": integer(value, "spi_mode", bounds=(0, 3)),
        "lcd_id": integer(value, "lcd_id", bounds=(0, 0xFFFFFFFF)),
        "exec_distance": integer(value, "exec_distance", bounds=(0, 0xFFFF)),
        "backlight_channels": backlight_channels(value, "backlight_channels"),
        "backlight_level": integer(value, "backlight_level", bounds=(0, 0x3F)),
        "session_name": text(value, "session_name"),
        "handoff_wait_seconds": integer(
            value,
            "handoff_wait_seconds",
            bounds=(1, 3600),
        ),
        "usb_release_wait_seconds": integer(
            value,
            "usb_release_wait_seconds",
            bounds=(1, 300),
        ),
        "boot_instructions": text(value, "boot_instructions"),
    }


def loader_arguments(
    loader: Path,
    fdl1: Path,
    image: Path,
    addresses: dict[str, int],
    exec_distance: int,
) -> list[str]:
    """Build the RAM-only loader command for one target."""
    arguments = [str(loader), "--wait", "0"]
    if exec_distance:
        arguments.extend(["t117_exec_dist", f"0x{exec_distance:x}"])
    arguments.extend(
        [
            "fdl",
            str(fdl1),
            f"0x{addresses['fdl1']:x}",
            "fdl",
            str(image),
            f"0x{addresses['payload']:x}",
        ]
    )
    return arguments


def backlight_argument(config: dict[str, Any]) -> str:
    """Return the validated libc_server backlight setting."""
    return f"{config['backlight_channels']}=0x{config['backlight_level']:x}"


def usb_device_path(vendor: int, product: int) -> Path | None:
    """Return the usbfs device node for one enumerated USB identity."""
    for vendor_file in Path("/sys/bus/usb/devices").glob("*/idVendor"):
        device = vendor_file.parent
        try:
            current_vendor = int(vendor_file.read_text().strip(), 16)
            current_product = int(vendor_file.with_name("idProduct").read_text().strip(), 16)
            bus = int((device / "busnum").read_text().strip())
            number = int((device / "devnum").read_text().strip())
        except FileNotFoundError, PermissionError, ValueError:
            continue
        if (current_vendor, current_product) == (vendor, product):
            return Path(f"/dev/bus/usb/{bus:03d}/{number:03d}")
    return None


def require_usb_device_access(device: Path, identity: str) -> None:
    """Wait briefly for uaccess while preserving exact USB failure diagnostics."""
    deadline = time.monotonic() + 0.25
    while True:
        try:
            descriptor = os.open(device, os.O_RDWR | os.O_CLOEXEC)
        except FileNotFoundError:
            raise BootromDisconnectedError from None
        except PermissionError:
            if time.monotonic() >= deadline:
                fail(
                    f"BootROM USB {identity} is not readable and writable by the current user; "
                    "install the documented udev rule and reconnect the phone"
                )
            time.sleep(0.01)
        except OSError as error:
            detail = error.strerror or str(error)
            fail(f"BootROM USB {identity} cannot be opened: {detail}")
        else:
            os.close(descriptor)
            return


def stop(process: subprocess.Popen[Any] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def wait_for_bootrom_disconnect(device: Path, wait_seconds: int) -> None:
    """Require the exact BootROM usbfs node to disappear after bridge acknowledgement."""
    deadline = time.monotonic() + wait_seconds
    while device.exists():
        if time.monotonic() >= deadline:
            fail("BootROM USB did not disconnect before the deadline")
        time.sleep(0.1)


def complete_linux_handoff(
    runtime: dict[str, Any],
    session: dict[str, Any],
    linux_usb: dict[str, int],
) -> None:
    """Finish one bridge-acknowledged transition through its declared transport."""
    if runtime["transport"] == "none":
        print("Bridge acknowledged the Linux transition; no host-side transport is selected.")
        return

    linux_id = f"{linux_usb['vendor_id']:04x}:{linux_usb['product_id']:04x}"
    print(
        "Bridge acknowledged the Linux transition; waiting up to "
        f"{linux_usb['wait_seconds']} seconds for Linux USB-NCM {linux_id}.",
        flush=True,
    )
    transport_module = importlib.import_module("ssh_transport")
    ready = transport_module.wait_for_bound_session(session)
    print("Private USB-NCM SSH session is ready.", flush=True)
    if not os.isatty(0):
        print("No interactive terminal is attached; the loader is complete.", flush=True)
        return
    transport_module.open_shell(ready)
    fail("SSH client returned without replacing the runner")


def run(
    bundle: Path,
    runtime: dict[str, Any],
    session: dict[str, Any],
) -> None:
    """Execute the fixed RAM-only UMS9117 sequence for one declared transport."""
    display_name = runtime_target_display_name(runtime)
    session_token = prepared_session_token(session)
    assets = runtime["assets"]
    if set(assets) != {"fdl1", "pinmap", "keymap"}:
        fail("runtime assets do not match the UMS9117 platform contract")
    pinmap = Path(assets["pinmap"])
    keymap = Path(assets["keymap"])
    if (
        pinmap.name != "pinmap.bin"
        or keymap.name != "keymap.bin"
        or pinmap.parent != keymap.parent
    ):
        fail("platform map assets must share a directory and fixed protocol names")
    tools = runtime["host_tools"]
    if set(tools) != {"loader", "bridge", "keyboard"}:
        fail("runtime host tools do not match the UMS9117 platform contract")
    config = adapter_config(runtime["adapter"])
    stdbuf = shutil.which("stdbuf")
    if stdbuf is None:
        fail("GNU stdbuf is required (install coreutils before starting the RAM loader)")

    image = Path(session["image"])
    fdl1 = bundle / assets["fdl1"]
    loader = bundle / tools["loader"]
    bridge = bundle / tools["bridge"]
    addresses = runtime["addresses"]
    bootrom_usb = runtime["usb"]["bootrom"]
    linux_usb = runtime["usb"]["linux_gadget"]

    loader_argv = loader_arguments(
        loader,
        fdl1,
        image,
        addresses,
        config["exec_distance"],
    )
    bootrom_id = f"{bootrom_usb['vendor_id']:04x}:{bootrom_usb['product_id']:04x}"
    print(f"{display_name} console RAM boot")
    operations = ["RAM FDL1 load", "RAM payload load"]
    if config["exec_distance"]:
        operations.insert(0, "exec-distance setup")
    print(f"Operations: {', '.join(operations)}.")
    print("There are no flash, erase, partition, or NV commands.")
    print()
    print(config["boot_instructions"])
    print(
        f"Waiting up to {bootrom_usb['wait_seconds']} seconds for BootROM USB {bootrom_id}...",
        flush=True,
    )
    deadline = time.monotonic() + bootrom_usb["wait_seconds"]
    max_loader_attempts = 3
    successful_bootrom_device: Path | None = None
    try:
        for attempt in range(1, max_loader_attempts + 1):
            while True:
                bootrom_device = usb_device_path(
                    bootrom_usb["vendor_id"],
                    bootrom_usb["product_id"],
                )
                while bootrom_device is None:
                    if time.monotonic() >= deadline:
                        fail("BootROM USB was not detected")
                    time.sleep(0.01)
                    bootrom_device = usb_device_path(
                        bootrom_usb["vendor_id"],
                        bootrom_usb["product_id"],
                    )
                try:
                    require_usb_device_access(bootrom_device, bootrom_id)
                except BootromDisconnectedError:
                    if time.monotonic() >= deadline:
                        fail(
                            f"BootROM USB {bootrom_id} repeatedly disconnected "
                            "before it could be opened"
                        )
                    time.sleep(0.01)
                    continue
                break
            result = subprocess.run(loader_argv, check=False)
            if result.returncode == 0:
                successful_bootrom_device = bootrom_device
                break
            if attempt == max_loader_attempts:
                fail(
                    f"RAM loader failed after {max_loader_attempts} attempts; "
                    f"last exit status {result.returncode}"
                )
            print(
                f"RAM loader attempt {attempt}/{max_loader_attempts} exited with "
                f"status {result.returncode}. Disconnect and power off the phone, "
                "then reconnect it in BootROM mode; waiting for a fresh USB device.",
                flush=True,
            )
            while bootrom_device.exists():
                if time.monotonic() >= deadline:
                    fail("BootROM USB did not disconnect before the retry deadline")
                time.sleep(0.01)
    finally:
        transport_module = importlib.import_module("ssh_transport")
        transport_module.remove_personalized_image(session)
    if successful_bootrom_device is None:
        fail("RAM loader completed without a BootROM USB device")

    bridge_argv = [
        stdbuf,
        "-oL",
        "-eL",
        str(bridge),
    ]
    bridge_argv.extend(
        [
            "--fplinux-handoff",
            session_token,
            "--",
            "--bright",
            str(config["brightness"]),
            "--rotate",
            str(config["rotation"]),
            "--spi_mode",
            str(config["spi_mode"]),
            "--lcd",
            f"0x{config['lcd_id']:x}",
            "--bl_extra",
            backlight_argument(config),
            config["session_name"],
        ]
    )
    bridge_process: subprocess.Popen[Any] | None = None
    try:
        bridge_process = subprocess.Popen(
            bridge_argv,
            cwd=bundle / Path(assets["pinmap"]).parent,
        )
        try:
            status = bridge_process.wait(timeout=config["handoff_wait_seconds"])
        except subprocess.TimeoutExpired:
            fail("bridge did not acknowledge the Linux transition before the deadline")
        if status:
            fail(f"bridge did not acknowledge the Linux transition (status {status})")
    finally:
        stop(bridge_process)

    wait_for_bootrom_disconnect(
        successful_bootrom_device,
        config["usb_release_wait_seconds"],
    )
    complete_linux_handoff(runtime, session, linux_usb)
