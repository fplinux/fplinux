# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic bundled loaders and independently encoded RAM session inputs."""

from __future__ import annotations

import base64
import hashlib
import importlib.util
import struct
import zlib
from typing import TYPE_CHECKING, Any

from tests.fdt import binary_tree

if TYPE_CHECKING:
    from pathlib import Path
    from types import ModuleType


def load_program(name: str, path: Path) -> ModuleType:
    """Import the delivered Python file through its normal module loader."""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise AssertionError(path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def loader_images() -> dict[str, bytes]:
    """Supply distinct ready-to-upload stage bytes without private phone inputs."""
    return {
        "fdl1": b"DHTBfirst-stage",
        "fdl2": b"DHTBboard-initializer",
        "sml": b"secure-monitor",
    }


def loader_config() -> dict[str, Any]:
    """Declare the fixed RAM addresses independently of stage contents."""
    return {
        "fdl1_load": 0x5000,
        "sml_load": 0x8E000000,
        "fdl2_load": 0x9EFFFE00,
        "stack_guard_start": 0x808E0000,
        "stack_guard_end": 0x808F0000,
    }


def session_record() -> bytes:
    """Construct the fixed ABI using test-owned literal offsets and a valid public key."""
    record = bytearray(512)
    record[:8] = b"FPLSESS\0"
    struct.pack_into("<I", record, 12, 512)
    record[16:48] = bytes(range(1, 33))
    record[48:112] = bytes(range(64))
    record[112:180] = base64.b64encode(b"\0\0\0\x0bssh-ed25519\0\0\0\x20" + b"k" * 32)
    serial = hashlib.sha256(record[16:48]).hexdigest()[:32]
    configuration = f"usb_serial={serial}\nphone_address=10.23.45.2\n".encode("ascii")
    record[180:436] = configuration.ljust(256, b"\0")
    struct.pack_into("<I", record, 508, zlib.crc32(record[:508]))
    return bytes(record)


def session_tree(
    *, seed_path: str = "chosen", duplicate: bool = False, external_root: bool = False
) -> bytes:
    """Encode exact property markers with same-byte decoys outside those properties."""
    chosen = [
        ("rng-seed", b"\xa1" * 64),
    ]
    if external_root:
        chosen.append(
            ("bootargs", b"root=PARTUUID=46504c52-02 rootfstype=ext4 rw init=/sbin/init\0")
        )
    else:
        chosen.extend(
            [
                ("linux,initrd-start", bytes.fromhex("85500000")),
                ("linux,initrd-end", bytes.fromhex("85500007")),
            ]
        )
    if duplicate:
        chosen.append(("rng-seed", b"\xa1" * 64))
    return binary_tree(
        [("decoy", b"\xa1" * 64 + b"\xc3" * 32 + b"\xb2" * 68 + b"\xd4" * 256)],
        [
            (seed_path, chosen, []),
            (
                "fplinux-session",
                [
                    ("compatible", b"fplinux,ram-session\0"),
                    ("fplinux,session-id", b"\xc3" * 32),
                    ("fplinux,ssh-client-key", b"\xb2" * 68),
                    ("fplinux,usb-session", b"\xd4" * 256),
                ],
                [],
            ),
        ],
    )


def prepare_runtime(
    root: Path, *, external_root: bool = False
) -> tuple[dict[str, Any], dict[str, Any], Path]:
    """Create one owned bundle, loader assets and personalized compact image."""
    assets = root / "assets"
    assets.mkdir()
    images = loader_images()
    for role, data in images.items():
        (assets / f"{role}.bin").write_bytes(data)
    (root / "runner").mkdir()
    record = session_record()
    image = bytearray()
    regions = {}
    parts = [
        ("entry", b"entry123", 0x80002060),
        ("kernel", b"kernel123", 0x81008000),
        ("dtb", session_tree(external_root=external_root), 0x85400000),
    ]
    if not external_root:
        parts.append(("initramfs", b"root123", 0x85500000))
    for role, data, address in parts:
        image.extend(bytes(-len(image) % 64))
        regions[role] = {"offset": len(image), "size": len(data), "load_address": address}
        image.extend(data)
    image.extend(bytes(-len(image) % 64))
    offset = len(image)
    image.extend(record)
    (root / "ramboot.bin").write_bytes(image)
    session = {
        "image": str(root / "ramboot.bin"),
        "session_id": record[16:48].hex(),
        "usb_serial": hashlib.sha256(record[16:48]).hexdigest()[:32],
        "vendor_id": 0x0525,
        "product_id": 0xA4A6,
        "wait_seconds": 1,
    }
    runtime = {
        "identity": {"platform": {"name": "sc9820e"}, "target": {"display_name": "Test phone"}},
        "transport": "usb-ncm",
        "assets": {"fdl1": "assets/fdl1.bin", "sml": "assets/sml.bin", "fdl2": "assets/fdl2.bin"},
        "host_tools": {"loader": "loader"},
        "personalization": {"offset": offset, "bytes": 512},
        "addresses": {"fdl1": 0x5000, "payload": 0x80002060},
        "usb": {"bootrom": {"vendor_id": 0x1782, "product_id": 0x4D00, "wait_seconds": 1}},
        "adapter": {
            "session_name": "test-linux",
            "boot_instructions": "Connect the test phone.",
            "handoff_wait_seconds": 1,
            "usb_release_wait_seconds": 1,
            "regions": regions,
            "loader": loader_config(),
        },
    }
    return runtime, session, assets
