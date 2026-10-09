# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic partition input fixtures."""

from __future__ import annotations

import struct

from fplinux_cli.device_data import formats as device_data

VBM_COPY_OFFSETS = (0x20000, 0x40000)


class FakeNandPartitionReader:
    """Replace physical storage with sparse main-area fragments and block admission.

    Unwritten bytes read as erased. This fake does not decode physical OOB markers.
    """

    def __init__(
        self,
        fragments: dict[int, bytes],
        *,
        block_count: int = 1024,
        bad_blocks: frozenset[int] = frozenset(),
    ) -> None:
        """Retain only the fragments and capacity exposed to the parser."""
        self._fragments = fragments
        self._block_count = block_count
        self._bad_blocks = bad_blocks

    @property
    def block_count(self) -> int:
        """Return the declared fake physical capacity."""
        return self._block_count

    def main_bytes(self, offset: int, size: int) -> bytes:
        """Read overlapping fragments over the fake NAND's erased main area."""
        if offset < 0 or size < 0 or offset + size > self._block_count * 0x20000:
            message = "fake NAND read exceeds its declared capacity"
            raise ValueError(message)
        output = bytearray(b"\xff" * size)
        for fragment_offset, fragment in self._fragments.items():
            first = max(offset, fragment_offset)
            last = min(offset + size, fragment_offset + len(fragment))
            if first < last:
                output[first - offset : last - offset] = fragment[
                    first - fragment_offset : last - fragment_offset
                ]
        return bytes(output)

    def partition_bytes(self, extent: tuple[int, int]) -> bytes:
        """Return the requested bytes from one declared main-area fragment."""
        offset, size = extent
        for block in range(offset // 0x20000, (offset + size - 1) // 0x20000 + 1):
            if block in self._bad_blocks:
                raise ValueError(f"selected NAND block {block} is marked bad")
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


def parti_entries() -> tuple[tuple[int, int, int, int], ...]:
    """Use a short contiguous table with all four consumer-owned partitions."""
    return (
        (0x00000001, 0x100, 0, 2),
        (0x10000001, 0x100, 2, 2),
        (0x1000000F, 0x100, 4, 1),
        (0x10000018, 0x100, 5, 3),
        (0x10000003, 0x001, 8, 2),
        (0x00000008, 0x001, 10, 0xFFFFFFFF),
    )


def parti_table(
    entries: tuple[tuple[int, int, int, int], ...],
    *,
    declared_count: int | None = None,
) -> bytes:
    """Encode the compiled PartI count and fixed-width records."""
    count = len(entries) if declared_count is None else declared_count
    return struct.pack("<I", count) + b"".join(struct.pack("<4I", *entry) for entry in entries)
