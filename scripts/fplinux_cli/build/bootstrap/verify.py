# SPDX-License-Identifier: GPL-2.0-only
"""Validate RAM-session markers and the compiled bootstrap payload limits."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

from fplinux_cli.build.device_tree import DeviceTreeError, exact_path_properties
from fplinux_cli.common import fail, sha256_bytes

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.build.storage.uboot import UbootBuild


RAM_SESSION_BYTES = 512


RAM_SESSION_ALIGNMENT = 64


RAM_SESSION_RNG_SEED_MARKER = bytes([0xA1]) * 64


RAM_SESSION_DTB_MARKERS = {
    "fplinux,ssh-client-key": bytes([0xB2]) * 68,
    "fplinux,session-id": bytes([0xC3]) * 32,
    "fplinux,usb-session": bytes([0xD4]) * 256,
}


def _verify_session_dtb(tree: bytes) -> None:
    """Require one canonical marker for each RAM-session DT property."""
    try:
        properties = exact_path_properties(tree, ("/chosen", "/fplinux-session"))
    except DeviceTreeError as error:
        fail(str(error))
    chosen = properties["/chosen"]
    session = properties["/fplinux-session"]
    if chosen.get("rng-seed") != RAM_SESSION_RNG_SEED_MARKER:
        fail("target DTB /chosen rng-seed does not contain its canonical marker")
    if tree.count(RAM_SESSION_RNG_SEED_MARKER) != 1:
        fail("target DTB marker rng-seed must occur exactly once")
    if session.get("compatible") != b"fplinux,ram-session\0":
        fail("target DTB /fplinux-session has an invalid compatible")
    for name, marker in RAM_SESSION_DTB_MARKERS.items():
        if session.get(name) != marker:
            fail(f"target DTB /fplinux-session {name} does not contain its canonical marker")
        if tree.count(marker) != 1:
            fail(f"target DTB marker {name} must occur exactly once")


def verify_images(  # noqa: PLR0913 -- artifacts and target limits stay explicit.
    ramboot: Path,
    *,
    zimage: Path,
    dtb: Path,
    map_file: Path,
    load_address: int,
    payload_limit: int,
    forbidden_markers: list[str],
) -> dict[str, int | str]:
    """Enforce the generic RAM-only bootstrap image contract."""
    image = ramboot.read_bytes()
    kernel = zimage.read_bytes()
    tree = dtb.read_bytes()
    if image[:4] != b"DHTB" or len(image) < 0x200:
        fail("bootstrap output does not have a complete DHTB header")
    declared = struct.unpack_from("<I", image, 0x30)[0]
    if declared + 0x200 > len(image):
        fail("DHTB declared size exceeds the RAM image")
    if load_address + len(image) >= payload_limit:
        fail("RAM image reaches its declared payload limit")
    if len(kernel) < 0x28 or kernel[0x24:0x28] != b"\x18\x28\x6f\x01":
        fail("zImage has invalid ARM magic")
    if tree[:4] != b"\xd0\x0d\xfe\xed":
        fail("target DTB has invalid FDT magic")
    lowered = tree.lower()
    for marker in forbidden_markers:
        if marker.lower().encode() in lowered:
            fail(f"target DTB contains forbidden storage marker {marker}")

    symbols = _bootstrap_map_symbols(map_file)
    required = {
        "__image_start",
        "linux_zimage_start",
        "linux_zimage_end",
        "linux_dtb_start",
        "linux_dtb_end",
        "fplinux_session_start",
        "fplinux_session_end",
        "FPLINUX_BOOTSTRAP_STORAGE_DISABLED",
    }
    missing = sorted(required - symbols.keys())
    if missing:
        fail(f"bootstrap map lacks symbols: {', '.join(missing)}")
    if symbols["FPLINUX_BOOTSTRAP_STORAGE_DISABLED"] != 1:
        fail("bootstrap storage-disabled link marker is not one")
    if symbols["__image_start"] != load_address:
        fail("bootstrap map image start differs from target load_address")

    zimage_start = symbols["linux_zimage_start"]
    zimage_end = symbols["linux_zimage_end"]
    dtb_start = symbols["linux_dtb_start"]
    dtb_end = symbols["linux_dtb_end"]
    for name, start, end in (
        ("zImage", zimage_start, zimage_end),
        ("DTB", dtb_start, dtb_end),
    ):
        if not load_address <= start <= end <= load_address + len(image):
            fail(f"embedded {name} range lies outside the RAM image")
    if zimage_end - zimage_start != len(kernel):
        fail("embedded zImage size differs from the built zImage")
    if dtb_end - dtb_start != len(tree):
        fail("embedded DTB size differs from the built DTB")
    if image[zimage_start - load_address : zimage_end - load_address] != kernel:
        fail("embedded zImage bytes differ from the built zImage")
    if image[dtb_start - load_address : dtb_end - load_address] != tree:
        fail("embedded DTB bytes differ from the built DTB")

    session_start = symbols["fplinux_session_start"]
    session_end = symbols["fplinux_session_end"]
    expected_start = (dtb_end + RAM_SESSION_ALIGNMENT - 1) & -RAM_SESSION_ALIGNMENT
    if session_start != expected_start or session_start % RAM_SESSION_ALIGNMENT:
        fail("RAM-session slot does not immediately follow the aligned embedded DTB")
    if session_end - session_start != RAM_SESSION_BYTES:
        fail("RAM-session slot does not have its exact ABI size")
    if not load_address <= session_start < session_end <= load_address + len(image):
        fail("RAM-session slot lies outside the RAM image")
    offset = session_start - load_address
    template = image[offset : offset + RAM_SESSION_BYTES]
    if template != bytes(RAM_SESSION_BYTES):
        fail("canonical RAM-session slot is not all zero")
    _verify_session_dtb(tree)
    return {
        "offset": offset,
        "bytes": RAM_SESSION_BYTES,
        "template_sha256": sha256_bytes(template),
    }


def _bootstrap_map_symbols(map_file: Path) -> dict[str, int]:
    symbols: dict[str, int] = {}
    for line in map_file.read_text().splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        try:
            symbols[fields[2]] = int(fields[0], 16)
        except ValueError:
            continue
    return symbols


def verify_sd_stage0_image(  # noqa: PLR0913 -- artifacts and target limits stay explicit.
    ramboot: Path,
    uboot: UbootBuild,
    *,
    map_file: Path,
    load_address: int,
    payload_limit: int,
    layout: dict[str, int],
) -> dict[str, int | str]:
    """Verify the resident stage0, embedded U-Boot and session slot."""
    image = ramboot.read_bytes()
    binary = uboot.binary.read_bytes()
    if image[:4] != b"DHTB" or len(image) < 0x200:
        fail("U-Boot stage0 output does not have a complete DHTB header")
    declared = struct.unpack_from("<I", image, 0x30)[0]
    if declared + 0x200 > len(image):
        fail("U-Boot stage0 DHTB declared size exceeds the RAM image")
    if load_address + len(image) >= payload_limit:
        fail("U-Boot stage0 reaches its declared payload limit")

    symbols = _bootstrap_map_symbols(map_file)
    required = {
        "__image_start",
        "__bss_end",
        "uboot_payload_start",
        "uboot_payload_end",
        "fplinux_session_start",
        "fplinux_session_end",
        "stage0_ops",
        "uboot_handoff",
        "FPLINUX_BOOTSTRAP_STORAGE_DISABLED",
    }
    missing = sorted(required - symbols.keys())
    if missing:
        fail(f"U-Boot stage0 map lacks symbols: {', '.join(missing)}")
    if symbols["FPLINUX_BOOTSTRAP_STORAGE_DISABLED"] != 1:
        fail("U-Boot stage0 storage-disabled marker is not one")
    if symbols["__image_start"] != load_address:
        fail("U-Boot stage0 map image start differs from target load_address")
    if symbols["__bss_end"] > layout["uboot_stack"]:
        fail("resident U-Boot stage0 overlaps the full U-Boot stack")

    uboot_start = symbols["uboot_payload_start"]
    uboot_end = symbols["uboot_payload_end"]
    if not load_address <= uboot_start < uboot_end <= load_address + len(image):
        fail("embedded full U-Boot range lies outside stage0")
    if uboot_end - uboot_start != len(binary):
        fail("embedded full U-Boot size differs from its verified binary")
    if image[uboot_start - load_address : uboot_end - load_address] != binary:
        fail("embedded full U-Boot bytes differ from its verified binary")

    session_start = symbols["fplinux_session_start"]
    session_end = symbols["fplinux_session_end"]
    expected_start = (uboot_end + RAM_SESSION_ALIGNMENT - 1) & -RAM_SESSION_ALIGNMENT
    if session_start != expected_start or session_start % RAM_SESSION_ALIGNMENT:
        fail("stage0 session slot does not immediately follow embedded U-Boot")
    if session_end - session_start != RAM_SESSION_BYTES:
        fail("stage0 session slot does not have its exact ABI size")
    offset = session_start - load_address
    template = image[offset : offset + RAM_SESSION_BYTES]
    if template != bytes(RAM_SESSION_BYTES):
        fail("canonical stage0 session slot is not all zero")
    for name in ("stage0_ops", "uboot_handoff"):
        if not load_address <= symbols[name] < symbols["__bss_end"]:
            fail(f"resident {name} lies outside the stage0 image")
    return {
        "offset": offset,
        "bytes": RAM_SESSION_BYTES,
        "template_sha256": sha256_bytes(template),
    }
