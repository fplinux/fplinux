# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Standalone SC9820E RAM uploads and authenticated Linux handoff."""

from __future__ import annotations

import base64
import binascii
import contextlib
import hashlib
import os
import stat
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path
from typing import TYPE_CHECKING, Any, NoReturn

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import TextIO

USB_DEVICES = Path("/sys/bus/usb/devices")
USB_NODES = Path("/dev/bus/usb")
ROLES = ("entry", "kernel", "dtb", "initramfs")


def fail(message: str) -> NoReturn:
    raise SystemExit(f"fplinux run: {message}")


def _integer(
    table: dict[str, Any], key: str, *, minimum: int = 0, maximum: int = 0xFFFFFFFF
) -> int:
    value = table.get(key)
    if type(value) is not int or not minimum <= value <= maximum:
        fail(f"adapter {key} must be an integer in {minimum}..{maximum}")
    return value


def adapter_config(value: object) -> dict[str, Any]:
    """Validate the fixed platform runtime and RAM loader bounds."""
    keys = {
        "session_name",
        "boot_instructions",
        "handoff_wait_seconds",
        "usb_release_wait_seconds",
        "regions",
        "loader",
    }
    if not isinstance(value, dict) or set(value) != keys:
        fail("adapter data does not match the SC9820E platform contract")
    for key in ("session_name", "boot_instructions"):
        if not isinstance(value[key], str) or not value[key]:
            fail(f"adapter {key} must be a non-empty string")
    _integer(value, "handoff_wait_seconds", minimum=1, maximum=3600)
    _integer(value, "usb_release_wait_seconds", minimum=1, maximum=300)
    regions = value["regions"]
    if not isinstance(regions, dict) or set(regions) not in (
        {"entry", "kernel", "dtb"},
        set(ROLES),
    ):
        fail("SC9820E requires entry, kernel, dtb and optional initramfs regions")
    for role, region in regions.items():
        if not isinstance(region, dict) or set(region) != {"offset", "size", "load_address"}:
            fail(f"SC9820E {role} region is invalid")
        _integer(region, "offset")
        _integer(region, "size", minimum=1)
        _integer(region, "load_address", minimum=1)
    loader = value["loader"]
    if not isinstance(loader, dict) or set(loader) != {
        "fdl1_load",
        "fdl2_load",
        "sml_load",
        "stack_guard_start",
        "stack_guard_end",
    }:
        fail("SC9820E loader metadata is invalid")
    for key in ("fdl1_load", "fdl2_load", "sml_load"):
        _integer(loader, key, minimum=1)
    start = _integer(loader, "stack_guard_start", minimum=1)
    if _integer(loader, "stack_guard_end", minimum=1) <= start:
        fail("SC9820E loader stack guard is empty or reversed")
    return value


def validate_runtime(runtime: dict[str, Any]) -> dict[str, Any]:
    """Require the fixed SC9820E RAM chain on either supported host."""
    config = adapter_config(runtime["adapter"])
    if runtime["identity"]["platform"]["name"] != "sc9820e" or runtime["transport"] != "usb-ncm":
        fail("SC9820E RAM boot requires its platform identity and authenticated USB transport")
    if runtime["assets"] != {
        "fdl1": "assets/fdl1.bin",
        "sml": "assets/sml.bin",
        "fdl2": "assets/fdl2.bin",
    } or set(runtime["host_tools"]) != {"loader"}:
        fail("SC9820E runtime assets or host tools do not match the fixed RAM chain")
    return config


def _session_record(image: bytes, runtime: dict[str, Any], session: dict[str, Any]) -> bytes:
    slot = runtime["personalization"]
    offset = _integer(slot, "offset", minimum=512)
    if slot.get("bytes") != 512 or offset % 64 or offset + 512 > len(image):
        fail("SC9820E session slot is outside the RAM image or misaligned")
    record = image[offset : offset + 512]
    if record[:8] != b"FPLSESS\0" or struct.unpack_from("<I", record, 12)[0] != 512:
        fail("SC9820E session record has an invalid header")
    if any(record[8:12]) or any(record[436:508]):
        fail("SC9820E session record has nonzero reserved bytes")
    if struct.unpack_from("<I", record, 508)[0] != zlib.crc32(record[:508]):
        fail("SC9820E session record checksum does not match")
    if not any(record[16:48]) or not any(record[48:112]):
        fail("SC9820E session record has empty identity or entropy")
    if record[16:48].hex() != session.get("session_id"):
        fail("SC9820E session record belongs to another prepared session")
    if hashlib.sha256(record[16:48]).hexdigest()[:32] != session.get("usb_serial"):
        fail("SC9820E session USB serial does not match the prepared session")
    try:
        key = base64.b64decode(record[112:180], validate=True)
    except binascii.Error:
        fail("SC9820E session SSH client key is invalid")
    if len(key) != 51 or key[:19] != b"\0\0\0\x0bssh-ed25519\0\0\0\x20" or not any(key[19:]):
        fail("SC9820E session SSH client key is invalid")
    configuration, separator, padding = record[180:436].partition(b"\0")
    if (
        not separator
        or not configuration.endswith(b"\n")
        or any(padding)
        or any(byte != 10 and not 0x20 <= byte <= 0x7E for byte in configuration)
        or f"usb_serial={session['usb_serial']}\n".encode("ascii")
        not in configuration.splitlines(keepends=True)
    ):
        fail("SC9820E USB session configuration is invalid")
    return record


def _property_ranges(tree: bytes) -> dict[tuple[str, str], tuple[int, int]]:
    """Parse bounded FDT structure tokens and retain exact property data ranges."""
    if len(tree) < 40:
        fail("SC9820E DTB header is truncated")
    magic, total, structure, strings, reserved, version, compatible, _cpu, string_size, size = (
        struct.unpack_from(">10I", tree)
    )
    if (
        magic != 0xD00DFEED
        or total != len(tree)
        or version < 17
        or compatible > 17
        or structure < 40
        or structure % 4
        or strings < 40
        or reserved < 40
        or reserved % 8
        or structure + size > total
        or strings + string_size > total
        or (structure < strings + string_size and strings < structure + size)
    ):
        fail("SC9820E DTB header or block bounds are invalid")
    reserve_end = reserved
    while True:
        if reserve_end + 16 > total:
            fail("SC9820E DTB memory reservation list is truncated")
        address, length = struct.unpack_from(">QQ", tree, reserve_end)
        reserve_end += 16
        if address == length == 0:
            break
    if any(
        reserved < end and start < reserve_end
        for start, end in ((structure, structure + size), (strings, strings + string_size))
    ):
        fail("SC9820E DTB reservation list overlaps another block")

    properties: dict[tuple[str, str], tuple[int, int]] = {}
    stack: list[str] = []
    seen_nodes: set[str] = set()
    position, end = structure, structure + size
    while position + 4 <= end:
        token = struct.unpack_from(">I", tree, position)[0]
        position += 4
        if token == 1:
            name_end = tree.find(b"\0", position, end)
            if name_end < 0:
                fail("SC9820E DTB node name is unterminated")
            try:
                name = tree[position:name_end].decode("ascii")
            except UnicodeDecodeError:
                fail("SC9820E DTB node name is invalid")
            if "/" in name or (not stack and (name or seen_nodes)) or (stack and not name):
                fail("SC9820E DTB node hierarchy is invalid")
            stack.append(name)
            path = "/".join(stack) or "/"
            if path in seen_nodes:
                fail("SC9820E DTB repeats a node")
            seen_nodes.add(path)
            position = (name_end + 4) & ~3
        elif token == 2:
            if not stack:
                fail("SC9820E DTB has an unmatched end node")
            stack.pop()
        elif token == 3:
            if not stack or position + 8 > end:
                fail("SC9820E DTB property header is invalid")
            length, name_offset = struct.unpack_from(">II", tree, position)
            position += 8
            if name_offset >= string_size or position + length > end:
                fail("SC9820E DTB property is outside its block")
            name_end = tree.find(b"\0", strings + name_offset, strings + string_size)
            if name_end < 0:
                fail("SC9820E DTB property name is unterminated")
            try:
                name = tree[strings + name_offset : name_end].decode("ascii")
            except UnicodeDecodeError:
                fail("SC9820E DTB property name is invalid")
            key = ("/".join(stack) or "/", name)
            if not name or key in properties:
                fail("SC9820E DTB repeats a property or has an empty name")
            properties[key] = position, length
            position = (position + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            if stack or position != end or "/" not in seen_nodes:
                fail("SC9820E DTB structure termination is invalid")
            return properties
        else:
            fail("SC9820E DTB contains an invalid structure token")
    fail("SC9820E DTB structure is truncated")


def personalize_dtb(
    tree: bytes,
    record: bytes,
    initrd: dict[str, int] | None,
    *,
    initramfs_bytes: int | None = None,
) -> bytes:
    """Replace four session properties while preserving the selected root handoff."""
    properties = _property_ranges(tree)
    replacements = (
        ("/chosen", "rng-seed", b"\xa1" * 64, record[48:112]),
        ("/fplinux-session", "fplinux,session-id", b"\xc3" * 32, record[16:48]),
        ("/fplinux-session", "fplinux,ssh-client-key", b"\xb2" * 68, record[112:180]),
        ("/fplinux-session", "fplinux,usb-session", b"\xd4" * 256, record[180:436]),
    )
    for path, name, marker, _replacement in replacements:
        location = properties.get((path, name))
        if (
            location is None
            or location[1] != len(marker)
            or tree[location[0] : location[0] + location[1]] != marker
        ):
            fail(f"SC9820E DTB {path} {name} is not its canonical session marker")
    if initrd is None:
        if any(
            ("/chosen", name) in properties for name in ("linux,initrd-start", "linux,initrd-end")
        ):
            fail("SC9820E external root DTB must not contain initramfs properties")
    else:
        for name, expected in (
            ("linux,initrd-start", initrd["load_address"]),
            ("linux,initrd-end", initrd["load_address"] + initrd["size"]),
        ):
            location = properties.get(("/chosen", name))
            if (
                location is None
                or location[1] != 4
                or tree[location[0] : location[0] + 4] != struct.pack(">I", expected)
            ):
                fail(f"SC9820E DTB {name} differs from the uploaded initramfs")
    result = bytearray(tree)
    for path, name, _marker, replacement in replacements:
        offset, length = properties[path, name]
        result[offset : offset + length] = replacement
    if initramfs_bytes is not None:
        if (
            initrd is None
            or not initrd["size"] <= initramfs_bytes <= 0xFFFFFFFF - initrd["load_address"]
        ):
            fail("SC9820E supplemental initramfs has invalid RAM bounds")
        offset, length = properties["/chosen", "linux,initrd-end"]
        result[offset : offset + length] = struct.pack(
            ">I", initrd["load_address"] + initramfs_bytes
        )
    return bytes(result)


def _write_private(path: Path, data: bytes) -> None:
    with path.open("xb") as stream:
        path.chmod(0o600)
        stream.write(data)


def _require_nonoverlapping(ranges: list[tuple[str, int, int]]) -> None:
    for index, (name, start, end) in enumerate(ranges):
        for other, other_start, other_end in ranges[index + 1 :]:
            if start < other_end and other_start < end:
                fail(f"SC9820E {name} overlaps {other}")


def prepare_uploads(
    bundle: Path,
    runtime: dict[str, Any],
    session: dict[str, Any],
    directory: Path,
    *,
    device_data: Path | None = None,
) -> dict[str, Path]:
    """Extract bounded regions and bundled loaders in one private lifetime."""
    config = runtime["adapter"]
    prepared: dict[str, bytes] = {}
    for role in ("fdl1", "sml", "fdl2"):
        path = bundle / runtime["assets"][role]
        if path.is_symlink() or not path.is_file():
            fail(f"bundled {role} loader is missing or is not a regular file")
        data = path.read_bytes()
        if not data:
            fail(f"bundled {role} loader is empty")
        prepared[role] = data
    image = Path(session["image"]).read_bytes()
    record = _session_record(image, runtime, session)
    regions = config["regions"]
    roles = tuple(role for role in ROLES if role in regions)
    capsule = b""
    if device_data is not None:
        if "initramfs" not in regions:
            fail("SC9820E external root does not accept a supplemental initramfs")
        if device_data.is_symlink() or not device_data.is_file():
            fail("SC9820E supplemental initramfs is missing or is not a regular file")
        capsule = device_data.read_bytes()
        if not capsule:
            fail("SC9820E supplemental initramfs is empty")
    initramfs_bytes = None
    if capsule:
        size = regions["initramfs"]["size"]
        initramfs_bytes = size + (-size % 4) + len(capsule)
    intervals = [
        (
            "session",
            runtime["personalization"]["offset"],
            runtime["personalization"]["offset"] + 512,
        )
    ]
    memory: list[tuple[str, int, int]] = []
    for role in roles:
        region = regions[role]
        start, end = region["offset"], region["offset"] + region["size"]
        if start % 64 or end > len(image) or region["load_address"] + region["size"] > 0x100000000:
            fail(f"SC9820E {role} region has invalid image or RAM bounds")
        intervals.append((role, start, end))
        uploaded_size = (
            initramfs_bytes
            if role == "initramfs" and initramfs_bytes is not None
            else region["size"]
        )
        if region["load_address"] + uploaded_size > 0x100000000:
            fail(f"SC9820E {role} exceeds the 32-bit RAM address range")
        memory.append((role, region["load_address"], region["load_address"] + uploaded_size))
    _require_nonoverlapping(intervals)
    if runtime["addresses"] != {
        "fdl1": config["loader"]["fdl1_load"],
        "payload": regions["entry"]["load_address"],
    }:
        fail("SC9820E runtime addresses differ from their loader and entry regions")
    for role, data in prepared.items():
        address = config["loader"][f"{role}_load"]
        end = address + len(data)
        if end > 0x100000000:
            fail(f"SC9820E {role} exceeds the 32-bit RAM address range")
        memory.append((role, address, end))
    memory.append(
        (
            "loader stack guard",
            config["loader"]["stack_guard_start"],
            config["loader"]["stack_guard_end"],
        )
    )
    _require_nonoverlapping(memory)
    uploads: dict[str, Path] = {}
    for role, data in prepared.items():
        uploads[role] = directory / f"{role}.bin"
        _write_private(uploads[role], data)
    for role in roles:
        region = regions[role]
        data = image[region["offset"] : region["offset"] + region["size"]]
        if role == "dtb":
            data = personalize_dtb(
                data, record, regions.get("initramfs"), initramfs_bytes=initramfs_bytes
            )
        elif role == "initramfs" and capsule:
            data += bytes(-len(data) % 4) + capsule
        uploads[role] = directory / f"{role}.bin"
        _write_private(uploads[role], data)
    return uploads


def _usb_candidates(vendor: int, product: int) -> list[Path]:
    matches = []
    for vendor_file in USB_DEVICES.glob("*/idVendor"):
        device = vendor_file.parent
        try:
            identity = (
                int(vendor_file.read_text(encoding="ascii").strip(), 16),
                int((device / "idProduct").read_text(encoding="ascii").strip(), 16),
            )
        except FileNotFoundError:
            continue
        except (OSError, UnicodeDecodeError, ValueError) as error:
            fail(f"cannot inspect USB identity: {error}")
        if identity == (vendor, product):
            matches.append(device)
    return matches


def _select_bootrom(
    usb: dict[str, int], *, deadline: float, on_wait: Callable[[], None] | None = None
) -> tuple[Path, int, int]:
    access_deadline: float | None = None
    while True:
        now = time.monotonic()
        if access_deadline is not None and now >= access_deadline:
            fail(
                "selected BootROM USB device is not readable and writable; "
                "install the documented udev rule and reconnect"
            )
        if now >= deadline:
            fail("BootROM USB was not detected before the deadline")
        matches = _usb_candidates(usb["vendor_id"], usb["product_id"])
        if len(matches) > 1:
            fail("more than one BootROM USB device is connected; leave only the selected phone")
        if matches:
            on_wait = None
            device = matches[0]
            try:
                bus = int((device / "busnum").read_text(encoding="ascii").strip())
                number = int((device / "devnum").read_text(encoding="ascii").strip())
                node = USB_NODES / f"{bus:03d}" / f"{number:03d}"
                if (
                    not 1 <= bus <= 255
                    or not 1 <= number <= 127
                    or not stat.S_ISCHR(node.stat().st_mode)
                ):
                    fail("selected BootROM USB address or device node is invalid")
                descriptor = os.open(node, os.O_RDWR | os.O_CLOEXEC)
                os.close(descriptor)
                if int((device / "devnum").read_text(encoding="ascii").strip()) != number:
                    fail("selected BootROM USB device changed before the RAM loader started")
            except FileNotFoundError:
                continue
            except PermissionError:
                if access_deadline is None:
                    # USB enumeration can precede udev permissions on the device node.
                    access_deadline = min(deadline, time.monotonic() + 1.0)
            except (OSError, UnicodeDecodeError, ValueError) as error:
                fail(f"cannot open selected BootROM USB device: {error}")
            else:
                return device, bus, number
        elif on_wait is not None:
            on_wait()
        sleep_deadline = access_deadline if access_deadline is not None else deadline
        time.sleep(max(0.0, min(0.05, sleep_deadline - time.monotonic())))


def loader_arguments(  # noqa: PLR0913 -- payload, USB selection and tracing remain explicit.
    loader: Path,
    uploads: dict[str, Path],
    config: dict[str, Any],
    *,
    bus: int | None,
    device: int | None,
    wait_seconds: int = 0,
    loader_trace: bool = False,
    working_directory: Path | None = None,
) -> list[str]:
    """Construct the RAM chain for exact selection or native unique-device discovery."""
    firmware = config["loader"]
    names = {
        role: str(path.relative_to(working_directory) if working_directory is not None else path)
        for role, path in uploads.items()
    }
    if (bus is None) != (device is None):
        fail("SC9820E USB bus and device must be selected together")
    selector = [] if bus is None else ["--usb-bus", str(bus), "--usb-device", str(device)]
    argv = [
        str(loader),
        "--wait",
        str(wait_seconds),
        *selector,
        "keep_charge",
        "1",
        "fdl",
        names["fdl1"],
        f"0x{firmware['fdl1_load']:x}",
    ]
    for role, address in (
        ("sml", firmware["sml_load"]),
        *(
            (role, config["regions"][role]["load_address"])
            for role in ("kernel", "dtb", "initramfs", "entry")
            if role in config["regions"]
        ),
    ):
        argv.extend(
            [
                "write_data",
                f"0x{address:x}",
                "0",
                str(uploads[role].stat().st_size),
                names[role],
            ]
        )
    argv.extend(["timeout", "60000", "fdl_exec", names["fdl2"], f"0x{firmware['fdl2_load']:x}"])
    if loader_trace:
        argv[1:1] = ["--verbose", "1"]
    return argv


def _stop(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    with contextlib.suppress(ProcessLookupError):
        process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(ProcessLookupError):
            process.kill()
        process.wait()


def _upload(argv: list[str], *, output: TextIO | None) -> None:
    process = subprocess.Popen(
        argv,
        stdin=subprocess.DEVNULL,
        stdout=output,
        stderr=subprocess.STDOUT if output is not None else None,
    )
    try:
        status = process.wait()
        if status:
            fail(
                f"RAM loader failed (exit status {status}); "
                "reconnect the powered-off phone before starting another run"
            )
    finally:
        _stop(process)


def _wait_linux_connection(selected: Path, session: dict[str, Any], *, deadline: float) -> None:
    while True:
        matches = _usb_candidates(session["vendor_id"], session["product_id"])
        exact = []
        for device in matches:
            try:
                if (device / "serial").read_text(encoding="ascii").strip() == session[
                    "usb_serial"
                ]:
                    exact.append(device)
            except FileNotFoundError:
                continue
            except (OSError, UnicodeDecodeError) as error:
                fail(f"cannot inspect Linux USB session: {error}")
        if len(exact) > 1 or (exact and exact[0] != selected):
            fail(
                "prepared Linux USB session appeared on a different "
                "or ambiguous physical connection"
            )
        if exact:
            return
        if time.monotonic() >= deadline:
            fail("prepared Linux USB session was not detected on the selected physical connection")
        time.sleep(0.05)


def run(  # noqa: PLR0913 -- shared runner inputs retain their standalone delivery contract.
    bundle: Path,
    runtime: dict[str, Any],
    session: dict[str, Any],
    *,
    expected_device_identity: str,
    events: Callable[[str], None] | None = None,
    device_data: Path | None = None,
    output: TextIO | None = None,
    loader_trace: bool = False,
    on_wait: Callable[[], None] | None = None,
) -> dict[str, Any]:
    """Upload once, observe the same USB connection and authenticate the Linux session."""
    from fplinux_cli.runtime import ssh_transport as transport  # noqa: PLC0415

    try:
        config = validate_runtime(runtime)
        identity = runtime["identity"]
        with tempfile.TemporaryDirectory(prefix="fplinux-sc9820e-") as temporary:
            uploads = prepare_uploads(
                bundle, runtime, session, Path(temporary), device_data=device_data
            )
            bootrom = runtime["usb"]["bootrom"]
            print(f"{identity['target']['display_name']} console RAM boot", flush=True)
            print(config["boot_instructions"], flush=True)
            print(
                f"Waiting up to {bootrom['wait_seconds']} seconds for BootROM USB "
                f"{bootrom['vendor_id']:04x}:{bootrom['product_id']:04x}...",
                flush=True,
            )
            if events is not None:
                events("waiting-for-device")
            selected, bus, number = _select_bootrom(
                bootrom, deadline=time.monotonic() + bootrom["wait_seconds"], on_wait=on_wait
            )
            if events is not None:
                events("device-found")
                events("ram-loader-start")
            _upload(
                loader_arguments(
                    bundle / runtime["host_tools"]["loader"],
                    uploads,
                    config,
                    bus=bus,
                    device=number,
                    loader_trace=loader_trace,
                ),
                output=output,
            )
            if events is not None:
                events("ram-loader-complete")
                events("linux-start")
            release_deadline = time.monotonic() + config["usb_release_wait_seconds"]
            while selected in _usb_candidates(bootrom["vendor_id"], bootrom["product_id"]):
                if time.monotonic() >= release_deadline:
                    fail("selected BootROM USB device did not disconnect after the final EXEC")
                time.sleep(0.05)
            if events is not None:
                events("linux-transition")
            handoff_deadline = time.monotonic() + config["handoff_wait_seconds"]
            _wait_linux_connection(selected, session, deadline=handoff_deadline)

            def observed() -> None:
                _wait_linux_connection(selected, session, deadline=handoff_deadline)
                if events is not None:
                    events("linux-usb")

            ready: dict[str, Any] = transport.wait_for_bound_session(
                session,
                expected_device_identity=expected_device_identity,
                on_linux_usb=observed,
            )
            _wait_linux_connection(selected, session, deadline=handoff_deadline)
            if events is not None:
                events("ssh-ready")
            print("Private USB-NCM SSH session is ready.", flush=True)
            return ready
    finally:
        transport.remove_personalized_image(session)
