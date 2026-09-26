# SPDX-License-Identifier: GPL-2.0-only
"""Shared physical NAND formats and target device-data results."""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from itertools import pairwise
from typing import TYPE_CHECKING, Protocol

if TYPE_CHECKING:
    from collections.abc import Mapping

PAGE_MAIN_BYTES = 2048
PHYSICAL_PAGE_COUNT = 65536
PAGES_PER_BLOCK = 64
BLOCK_MAIN_BYTES = PAGE_MAIN_BYTES * PAGES_PER_BLOCK
_VBM_MAX_PARTITIONS = 100
_VBM_HEADER_BYTES = 0x18
_VBM_ENTRY = struct.Struct("<IHHH")


class NandPartitionReader(Protocol):
    """Small read boundary shared by physical NAND and format-only tests."""

    @property
    def block_count(self) -> int:
        """Return the physical NAND capacity in erase blocks."""
        ...

    def partition_bytes(self, extent: tuple[int, int]) -> bytes:
        """Read one main-area extent whose selected blocks are admissible."""
        ...


@dataclass(frozen=True)
class NandGeometry:
    """Chip identity and physical layout reported by the running NAND reader."""

    id_bytes: str
    chip: str
    page_main_bytes: int
    oob_bytes: int
    pages_per_block: int
    block_count: int
    raw_bytes: int

    @property
    def raw_page_bytes(self) -> int:
        """Return the stored bytes per page: main area followed by OOB."""
        return self.page_main_bytes + self.oob_bytes


def family_page_bytes(geometry: NandGeometry) -> int:
    """Return the physical page size of a reported geometry that device data can interpret."""
    layout = (
        geometry.page_main_bytes,
        geometry.pages_per_block,
        geometry.pages_per_block * geometry.block_count,
    )
    if layout != (PAGE_MAIN_BYTES, PAGES_PER_BLOCK, PHYSICAL_PAGE_COUNT):
        raise ValueError(
            f"unsupported NAND geometry: {geometry.page_main_bytes}-byte main pages, "
            f"{geometry.pages_per_block} pages per block, {geometry.block_count} blocks; "
            f"expected {PAGE_MAIN_BYTES}-byte main pages, {PAGES_PER_BLOCK} pages per block "
            f"and {PHYSICAL_PAGE_COUNT} pages"
        )
    return geometry.raw_page_bytes


@dataclass(frozen=True)
class RequiredPartition:
    """A consumer-owned partition name and the attributes it requires."""

    name: str
    attributes: int


@dataclass(frozen=True)
class _VbmPartition:
    identifier: int
    attributes: int
    first_block: int
    last_block: int


@dataclass(frozen=True)
class PhysicalNand:
    """Main-area reads and factory-marker checks for one physical page geometry."""

    raw: bytes
    page_bytes: int

    @property
    def block_count(self) -> int:
        """Return the number of complete erase blocks in this physical image."""
        return len(self.raw) // self.page_bytes // PAGES_PER_BLOCK

    @classmethod
    def from_dump(cls, raw: bytes, *, page_bytes: int) -> PhysicalNand:
        """Require a complete physical backup before interpreting target partitions."""
        expected_size = PHYSICAL_PAGE_COUNT * page_bytes
        if len(raw) != expected_size:
            raise ValueError(
                f"expected a complete {expected_size}-byte physical NAND backup "
                f"({PAGE_MAIN_BYTES} main + {page_bytes - PAGE_MAIN_BYTES} OOB bytes per page), "
                f"got {len(raw)} bytes"
            )
        return cls(raw, page_bytes)

    def main_bytes(self, offset: int, size: int) -> bytes:
        """Read main-area bytes without including the intervening physical OOB."""
        available = len(self.raw) // self.page_bytes * PAGE_MAIN_BYTES
        if offset < 0 or size < 0 or offset + size > available:
            message = "partition extent is outside the physical NAND backup"
            raise ValueError(message)
        output = bytearray()
        while size:
            page, column = divmod(offset, PAGE_MAIN_BYTES)
            count = min(size, PAGE_MAIN_BYTES - column)
            position = page * self.page_bytes + column
            output.extend(self.raw[position : position + count])
            offset += count
            size -= count
        return bytes(output)

    def require_good_blocks(self, offset: int, size: int) -> None:
        """Reject selected bad-marked blocks instead of guessing BML replacements."""
        first = offset // BLOCK_MAIN_BYTES
        last = (offset + size - 1) // BLOCK_MAIN_BYTES
        for block in range(first, last + 1):
            for page_in_block in (0, 1):
                page = block * PAGES_PER_BLOCK + page_in_block
                if self.raw[page * self.page_bytes + PAGE_MAIN_BYTES] != 0xFF:
                    raise ValueError(
                        f"unsupported bad-block mapping: selected NAND block {block} is marked bad"
                    )

    def partition_bytes(self, extent: tuple[int, int]) -> bytes:
        """Read an admitted physical extent only when its factory markers are clean."""
        offset, size = extent
        contents = self.main_bytes(offset, size)
        self.require_good_blocks(offset, size)
        return contents


def _read_vbm_copy(
    nand: NandPartitionReader,
    offset: int,
) -> tuple[int, tuple[_VbmPartition, ...]]:
    header = nand.partition_bytes((offset, _VBM_HEADER_BYTES))
    if len(header) != _VBM_HEADER_BYTES:
        raise ValueError(f"truncated VBM header at {offset:#x}")
    if header[:8] != b"VBM_BOOT" or struct.unpack_from("<I", header, 8)[0] != 0x102:
        raise ValueError(f"unsupported or damaged VBM header at {offset:#x}")

    count = struct.unpack_from("<H", header, 0x16)[0]
    count_limit = min(_VBM_MAX_PARTITIONS, nand.block_count)
    if not 0 < count <= count_limit:
        raise ValueError(f"VBM partition count {count} is outside the 1..{count_limit} bound")
    entries_size = count * _VBM_ENTRY.size
    encoded = nand.partition_bytes((offset + _VBM_HEADER_BYTES, entries_size))
    if len(encoded) != entries_size:
        raise ValueError(f"truncated VBM partition table at {offset:#x}")
    entries = tuple(_VbmPartition(*values) for values in _VBM_ENTRY.iter_unpack(encoded))
    return count, entries


def redundant_vbm_partitions(
    nand: NandPartitionReader,
    copy_offsets: tuple[int, int],
    required: Mapping[int, RequiredPartition],
) -> dict[int, tuple[int, int]]:
    """Admit redundant structural VBM tables and their direct physical extents."""
    first_copy = _read_vbm_copy(nand, copy_offsets[0])
    second_copy = _read_vbm_copy(nand, copy_offsets[1])
    if first_copy != second_copy:
        message = "redundant VBM partition tables disagree"
        raise ValueError(message)

    _count, entries = first_copy
    entries_by_id: dict[int, _VbmPartition] = {}
    for entry in entries:
        if entry.identifier in entries_by_id:
            raise ValueError(f"duplicate VBM partition ID {entry.identifier:#010x}")
        block_count = entry.last_block - entry.first_block + 1
        if block_count <= 0:
            raise ValueError(
                f"VBM partition {entry.identifier:#010x} has a zero or reversed extent"
            )
        if entry.first_block >= nand.block_count or entry.last_block >= nand.block_count:
            raise ValueError(
                f"VBM partition {entry.identifier:#010x} is outside the "
                f"{nand.block_count}-block NAND"
            )
        entries_by_id[entry.identifier] = entry

    ordered = sorted(entries, key=lambda entry: entry.first_block)
    for previous, current in pairwise(ordered):
        if current.first_block <= previous.last_block:
            raise ValueError(
                f"VBM partitions {previous.identifier:#010x} and "
                f"{current.identifier:#010x} overlap"
            )

    admitted: dict[int, _VbmPartition] = {}
    for identifier, requirement in required.items():
        required_entry = entries_by_id.get(identifier)
        if required_entry is None:
            raise ValueError(
                f"required VBM partition {requirement.name} ({identifier:#010x}) is missing"
            )
        if required_entry.attributes != requirement.attributes:
            raise ValueError(
                f"required VBM partition {requirement.name} has attributes "
                f"{required_entry.attributes:#x}; expected {requirement.attributes:#x}"
            )
        admitted[identifier] = required_entry

    return {
        entry.identifier: (
            entry.first_block * BLOCK_MAIN_BYTES,
            (entry.last_block - entry.first_block + 1) * BLOCK_MAIN_BYTES,
        )
        for entry in admitted.values()
    }


def fixed_nv_records(partition: bytes, required_sizes: Mapping[int, int]) -> dict[int, bytes]:
    """Read selected exact-size records from one complete sorted fixed NV stream."""
    if len(partition) < 8:
        message = "incomplete fixed NV stream"
        raise ValueError(message)
    # The first word is a per-phone generation, not a fixed magic signature.
    cursor = 4
    previous_id = 0
    records: dict[int, bytes] = {}
    while cursor + 4 <= len(partition):
        identifier, length = struct.unpack_from("<HH", partition, cursor)
        cursor += 4
        if identifier == 0xFFFF and length == 0xFFFF:
            for required_id, required_size in required_sizes.items():
                if required_id not in records or len(records[required_id]) != required_size:
                    raise ValueError(
                        f"fixed NV record {required_id} is missing or has the wrong size"
                    )
            return records
        if identifier <= previous_id or identifier == 0xFFFF:
            message = "damaged fixed NV record ordering"
            raise ValueError(message)
        previous_id = identifier
        padded_length = (length + 3) & ~3
        if cursor + padded_length > len(partition):
            message = "truncated fixed NV record"
            raise ValueError(message)
        if identifier in required_sizes:
            records[identifier] = partition[cursor : cursor + length]
        cursor += padded_length
    message = "fixed NV stream has no complete terminator"
    raise ValueError(message)


@dataclass(frozen=True)
class PreparedGroup:
    """Original fitted records, the complete normalized output and review reports for one group.

    Reports describe the extraction for a person; builds never read them.
    """

    originals: dict[str, bytes]
    prepared: dict[str, bytes]
    reports: dict[str, bytes] = field(default_factory=dict)


@dataclass(frozen=True)
class DeviceDataPreparation:
    """All independently declared groups extracted from one physical NAND image."""

    groups: dict[str, PreparedGroup]
