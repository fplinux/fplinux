# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic partition input fixtures."""

from __future__ import annotations

import importlib.util
import struct
import sys
from typing import TYPE_CHECKING

from fplinux_cli.device_data import formats as device_data

from tests import ROOT

if TYPE_CHECKING:
    from types import ModuleType


VBM_COPY_OFFSETS = (0x20000, 0x40000)

NOKIA_PARTI_OFFSET = 0x63DE0


class FakeNandPartitionReader:
    """Replace a full physical dump with exact small main-area table fragments."""

    def __init__(self, fragments: dict[int, bytes], *, block_count: int = 1024) -> None:
        """Retain only the fragments and capacity exposed to the parser."""
        self._fragments = fragments
        self._block_count = block_count

    @property
    def block_count(self) -> int:
        """Return the declared fake physical capacity."""
        return self._block_count

    def partition_bytes(self, extent: tuple[int, int]) -> bytes:
        """Return the requested bytes from one declared main-area fragment."""
        offset, size = extent
        for fragment_offset, fragment in self._fragments.items():
            relative = offset - fragment_offset
            if 0 <= relative < len(fragment):
                return fragment[relative : relative + size]
        raise AssertionError(f"unexpected fake NAND read at {offset:#x} for {size} bytes")


def required_partitions() -> dict[int, device_data.RequiredPartition]:
    """Name the four consumers and their literal fitted-table attributes."""
    return {
        0x10000001: device_data.RequiredPartition("DownloadedNV", 0x100),
        0x1000000F: device_data.RequiredPartition("ProtectNV", 0x100),
        0x10000018: device_data.RequiredPartition("CM4", 0x100),
        0x10000003: device_data.RequiredPartition("RunningNV", 0x001),
    }


def vbm_table(
    entries: tuple[tuple[int, int, int, int], ...],
    *,
    peer_fields: bytes = bytes(10),
    declared_count: int | None = None,
) -> bytes:
    """Encode the documented VBM header and packed inclusive-block records."""
    if len(peer_fields) != 10:
        message = "peer_fields must cover offsets 0xc..0x15"
        raise ValueError(message)
    count = len(entries) if declared_count is None else declared_count
    return b"".join(
        (
            b"VBM_BOOT",
            struct.pack("<I", 0x102),
            peer_fields,
            struct.pack("<H", count),
            *(struct.pack("<IHHH", *entry) for entry in entries),
        )
    )


def vbm_entries() -> tuple[tuple[int, int, int, int], ...]:
    """Use literal non-overlapping records, including all four data consumers."""
    return (
        (0x00000001, 0x100, 0, 1),
        (0x10000001, 0x100, 4, 11),
        (0x1000000F, 0x100, 13, 20),
        (0x10000018, 0x100, 116, 123),
        (0x10000003, 0x001, 157, 204),
        (0x00000008, 0x001, 701, 980),
    )


def nokia_parti_entries() -> tuple[tuple[int, int, int, int], ...]:
    """Use the fitted 22-record PartI shape as an explicit structural fixture."""
    return (
        (0x00000001, 0x100, 0, 2),
        (0x00000002, 0x100, 2, 2),
        (0x10000001, 0x100, 4, 8),
        (0x10000002, 0x100, 12, 1),
        (0x1000000F, 0x100, 13, 8),
        (0x10000006, 0x100, 21, 25),
        (0x10000005, 0x100, 46, 62),
        (0x10000013, 0x100, 108, 8),
        (0x10000018, 0x100, 116, 8),
        (0x10000019, 0x100, 124, 1),
        (0x1000001A, 0x001, 125, 32),
        (0x10000003, 0x001, 157, 48),
        (0x00000003, 0x100, 205, 112),
        (0x00000004, 0x100, 317, 128),
        (0x10000004, 0x100, 445, 192),
        (0x10000007, 0x001, 637, 64),
        (0x1000001B, 0x101, 701, 6),
        (0x1000001C, 0x101, 707, 2),
        (0x1000001D, 0x101, 709, 1),
        (0x10000022, 0x101, 710, 10),
        (0x10000020, 0x101, 720, 160),
        (0x00000008, 0x001, 880, 0xFFFFFFFF),
    )


def nokia_parti_table(
    entries: tuple[tuple[int, int, int, int], ...],
    *,
    declared_count: int | None = None,
) -> bytes:
    """Encode the compiled PartI count and fixed-width records."""
    count = len(entries) if declared_count is None else declared_count
    return struct.pack("<I", count) + b"".join(struct.pack("<4I", *entry) for entry in entries)


def load_target_parser(target: str, filename: str) -> ModuleType:
    path = ROOT / "targets" / target / filename
    spec = importlib.util.spec_from_file_location(
        f"{target.replace('-', '_')}_firmware_formats", path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load target firmware module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module
