# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic NAND layouts and literal records for stock-image decoder and report tests."""

from __future__ import annotations

import importlib.util
import struct
import sys
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.device_data.formats import PhysicalNand

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]

PAGE_MAIN_BYTES = 2048
BLOCK_MAIN_BYTES = 64 * PAGE_MAIN_BYTES
BLOCKS = 16
# Every synthetic layout keeps the stock application image in blocks 2-5.
APPLICATION_OFFSET = 2 * BLOCK_MAIN_BYTES
VBM_ENTRIES = (
    (0x00000001, 0x100, 0, 1),
    (0x00000003, 0x100, 2, 5),
    (0x10000001, 0x100, 6, 7),
    (0x00000008, 0x001, 8, 13),
)

# One NAND configuration record, field by field at its documented offsets.
NAND_RECORD_21E5 = bytes.fromhex(
    "e521000000000000"  # 0x00 chip ID
    "00040000"  # 0x08 1024 blocks
    "01000000"  # 0x0c one plane
    "2b000000"  # 0x10 43 reserved blocks
    "0004 2000"  # 0x14 1024-byte sectors, 32 spare bytes per sector
    "0200 4000"  # 0x18 two sectors per page, 64 pages per block
    "01000000"  # 0x1c write count 1, bad page 0, bad sector 0, bad marker position 0
    "01ff0004"  # 0x20 bad marker length 1, safe data position -1, length 0, main ECC position 4
    "0c040101"  # 0x24 main ECC 12 bits, risk 4, spare ECC position 1, 1 bit
    "01000501"  # 0x28 spare ECC risk 1, bus width 0, address cycles 5, advance 1
    "14002c01"  # 0x2c tr 20, tw 300
    "d0075000"  # 0x30 te 2000, tf 80
    "00000000"  # 0x34 padding
)
NAND_TERMINATOR = bytes(0x21) + b"\xff" + bytes(0x38 - 0x22)
EXPECTED_21E5 = {
    "id": "0x21e5",
    "block_count": 1024,
    "plane_count": 1,
    "reserved_block_count": 43,
    "sector_bytes": 1024,
    "spare_bytes_per_sector": 32,
    "sectors_per_page": 2,
    "pages_per_block": 64,
    "write_count_per_page": 1,
    "bad_page_index": 0,
    "bad_sector_index": 0,
    "bad_marker_position": 0,
    "bad_marker_length": 1,
    "safe_data_position": -1,
    "safe_data_length": 0,
    "main_ecc_position": 4,
    "main_ecc_bits": 12,
    "main_ecc_risk_bits": 4,
    "spare_ecc_position": 1,
    "spare_ecc_bits": 1,
    "spare_ecc_risk_bits": 1,
    "bus_width": 0,
    "address_cycles": 5,
    "advance": 1,
    "tr_time": 20,
    "tw_time": 300,
    "te_time": 2000,
    "tf_time": 80,
}


def load_stock_image() -> ModuleType:
    path = ROOT / "platforms/ums9117/host/stock_image.py"
    spec = importlib.util.spec_from_file_location("ums9117_stock_image_under_test", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load the platform module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


STOCK_IMAGE = load_stock_image()


def signed_image(payload: bytes) -> bytes:
    """Prefix the DHTB header: magic, version 1 and the image size at 0x30."""
    header = bytearray(0x200)
    header[0:8] = b"DHTB" + struct.pack("<I", 1)
    header[0x30:0x34] = struct.pack("<I", len(payload))
    return bytes(header) + payload


def vbm_copy(entries: tuple[tuple[int, int, int, int], ...], peer: bytes) -> bytes:
    """Encode the VBM header, count and inclusive (id, attributes, first, last) records."""
    return b"".join(
        (
            b"VBM_BOOT",
            struct.pack("<I", 0x102),
            peer,
            struct.pack("<H", len(entries)),
            *(struct.pack("<IHHH", *entry) for entry in entries),
        )
    )


def backup(
    image: bytes,
    *,
    vbm: tuple[bytes, ...] = (),
    parti: tuple[int, bytes] | None = None,
) -> PhysicalNand:
    """Write main-area content into an erased flash and interleave 64 OOB bytes per page."""
    main = bytearray(b"\xff" * (BLOCKS * BLOCK_MAIN_BYTES))
    main[APPLICATION_OFFSET : APPLICATION_OFFSET + len(image)] = image
    for index, copy in enumerate(vbm):
        offset = (BLOCKS - len(vbm) + index) * BLOCK_MAIN_BYTES
        main[offset : offset + len(copy)] = copy
    if parti is not None:
        offset, table = parti
        main[offset : offset + len(table)] = table
    pages = (
        bytes(main[offset : offset + PAGE_MAIN_BYTES]) + b"\xff" * 64
        for offset in range(0, len(main), PAGE_MAIN_BYTES)
    )
    return PhysicalNand(b"".join(pages), PAGE_MAIN_BYTES + 64)


def agreeing_vbm() -> tuple[bytes, bytes]:
    return (vbm_copy(VBM_ENTRIES, b"first-copy"), vbm_copy(VBM_ENTRIES, b"secondcopy"))


# Keymap index to stock code: column 0 has rows 0 and 1 and an empty row 2;
# column 1 has rows 0 to 2, row 2 holding a code without a phone key.
SPARSE_KEYMAP = {0: 0x2A, 1: 0x31, 8: 0x0D, 9: 0x05, 10: 0x24}
SPARSE_MATRIX = [
    {"row": 0, "column": 0, "code": "0x2a", "key": "FPLINUX_KEY_STAR"},
    {"row": 0, "column": 1, "code": "0x0d", "key": "FPLINUX_KEY_OK"},
    {"row": 1, "column": 0, "code": "0x31", "key": "FPLINUX_KEY_1"},
    {"row": 1, "column": 1, "code": "0x05", "key": "FPLINUX_KEY_DOWN"},
    {"row": 2, "column": 1, "code": "0x24", "key": None},
]


def keymap(codes: dict[int, int]) -> bytes:
    """Encode 64 little-endian key codes by keymap index; other entries are empty."""
    return b"".join(struct.pack("<H", codes.get(index, 0xFFFF)) for index in range(64))
