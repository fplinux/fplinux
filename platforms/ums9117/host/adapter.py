# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Fixed UMS9117 host translation for the shared RAM runner."""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any, NoReturn

if TYPE_CHECKING:
    from types import FrameType

CAPABILITY = "fplinux.host.ums9117-ram/v1"
BACKLIGHT_CHANNELS = "rgbw"


def fail(message: str) -> NoReturn:
    raise SystemExit(f"RAM adapter failed: {message}")


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
        "handoff_marker",
        "handoff_wait_seconds",
        "release_wait_seconds",
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
        "handoff_marker": text(value, "handoff_marker"),
        "handoff_wait_seconds": integer(
            value,
            "handoff_wait_seconds",
            bounds=(1, 3600),
        ),
        "release_wait_seconds": integer(
            value,
            "release_wait_seconds",
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
    arguments = [str(loader)]
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
        except (FileNotFoundError, PermissionError, ValueError):
            continue
        if (current_vendor, current_product) == (vendor, product):
            return Path(f"/dev/bus/usb/{bus:03d}/{number:03d}")
    return None


def stop(process: subprocess.Popen[str] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def stream(process: subprocess.Popen[str], marker: str, marker_seen: threading.Event) -> None:
    if process.stdout is None:
        fail("bridge output pipe is missing")
    for line in process.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        if marker in line:
            marker_seen.set()


def run(bundle: Path, runtime: dict[str, Any]) -> None:
    """Execute the fixed RAM-only UMS9117 sequence."""
    if runtime.get("capability") != CAPABILITY:
        fail(f"runtime capability must be {CAPABILITY}")
    assets = runtime["assets"]
    if set(assets) != {"fdl1", "pinmap", "keymap"}:
        fail("runtime assets do not match the platform capability")
    pinmap = Path(assets["pinmap"])
    keymap = Path(assets["keymap"])
    if (
        pinmap.name != "pinmap.bin"
        or keymap.name != "keymap.bin"
        or pinmap.parent != keymap.parent
    ):
        fail("platform map assets must share a directory and fixed protocol names")
    tools = runtime["host_tools"]
    if set(tools) != {"loader", "bridge", "console"}:
        fail("runtime host tools do not match the platform capability")
    config = adapter_config(runtime["adapter"])
    stdbuf = shutil.which("stdbuf")
    if stdbuf is None:
        fail("GNU stdbuf is required (install coreutils before starting the RAM loader)")

    image = bundle / runtime["image"]
    fdl1 = bundle / assets["fdl1"]
    loader = bundle / tools["loader"]
    bridge = bundle / tools["bridge"]
    console = bundle / tools["console"]
    addresses = runtime["addresses"]
    bootrom_usb = runtime["usb"]["bootrom"]
    linux_usb = runtime["usb"]["linux_console"]

    loader_argv = loader_arguments(
        loader,
        fdl1,
        image,
        addresses,
        config["exec_distance"],
    )
    bootrom_id = f"{bootrom_usb['vendor_id']:04x}:{bootrom_usb['product_id']:04x}"
    print(f"{runtime['display_name']} console RAM boot")
    operations = ["RAM FDL1 load", "RAM payload load"]
    if config["exec_distance"]:
        operations.insert(0, "exec-distance setup")
    print(f"Operations: {', '.join(operations)}.")
    print("There are no flash, erase, partition, or NV commands.")
    print()
    print(config["boot_instructions"])
    print(f"Waiting up to {bootrom_usb['wait_seconds']} seconds for BootROM USB {bootrom_id}...")
    deadline = time.monotonic() + bootrom_usb["wait_seconds"]
    bootrom_device = usb_device_path(bootrom_usb["vendor_id"], bootrom_usb["product_id"])
    while bootrom_device is None:
        if time.monotonic() >= deadline:
            fail("BootROM USB was not detected")
        time.sleep(0.25)
        bootrom_device = usb_device_path(
            bootrom_usb["vendor_id"],
            bootrom_usb["product_id"],
        )
    time.sleep(2)
    if not os.access(bootrom_device, os.R_OK | os.W_OK):
        fail(
            f"BootROM USB {bootrom_id} is not readable and writable by the current user; "
            "install the documented udev rule and reconnect the phone"
        )

    result = subprocess.run(loader_argv, check=False)
    if result.returncode:
        fail(f"RAM loader exited with status {result.returncode}")

    bridge_argv = [
        stdbuf,
        "-oL",
        "-eL",
        str(bridge),
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
    bridge_process: subprocess.Popen[str] | None = None

    def handle_signal(_signum: int, _frame: FrameType | None) -> None:
        stop(bridge_process)
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)
    try:
        bridge_process = subprocess.Popen(
            bridge_argv,
            cwd=bundle / Path(assets["pinmap"]).parent,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        marker_seen = threading.Event()
        reader = threading.Thread(
            target=stream,
            args=(bridge_process, config["handoff_marker"], marker_seen),
            daemon=True,
        )
        reader.start()
        deadline = time.monotonic() + config["handoff_wait_seconds"]
        while not marker_seen.wait(0.1):
            status = bridge_process.poll()
            if status is not None:
                fail(f"bridge exited before Linux handoff (status {status})")
            if time.monotonic() >= deadline:
                fail("Linux handoff marker was not observed before the deadline")
        release_deadline = time.monotonic() + config["release_wait_seconds"]
        while bridge_process.poll() is None and time.monotonic() < release_deadline:
            time.sleep(0.1)
        stop(bridge_process)
        reader.join(timeout=2)
    finally:
        stop(bridge_process)

    linux_id = f"{linux_usb['vendor_id']:04x}:{linux_usb['product_id']:04x}"
    print(
        "Bootstrap released USB; waiting up to "
        f"{linux_usb['wait_seconds']} seconds for Linux console {linux_id}."
    )
    console_path = str(console)
    os.execv(
        console_path,
        [
            console_path,
            "--vid",
            f"{linux_usb['vendor_id']:04x}",
            "--pid",
            f"{linux_usb['product_id']:04x}",
            "--wait",
            str(linux_usb["wait_seconds"]),
            "--interface",
            "0",
        ],
    )
