# SPDX-License-Identifier: GPL-2.0-only
"""Extract shared UMS9117 Bluetooth formats from physical NAND backups.

Targets select their partition layout and exact CM4 revision. Individual NV
values come from the backup; this reader does not remap blocks or recover ECC.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass

PAGE_MAIN_BYTES = 2048
PAGES_PER_BLOCK = 64
BLOCK_MAIN_BYTES = PAGE_MAIN_BYTES * PAGES_PER_BLOCK

NV_FILES = {
    401: ("bt-config.bin", 8),
    402: ("bt-sprd.bin", 176),
    404: ("bt-rf-config.bin", 252),
}


@dataclass(frozen=True)
class FirmwarePreparation:
    """Unchanged extracted originals and the complete build-ready input set."""

    originals: dict[str, bytes]
    prepared: dict[str, bytes]


@dataclass(frozen=True)
class PhysicalNand:
    """Main-area reads and factory-marker checks for one physical page geometry."""

    raw: bytes
    page_bytes: int

    @classmethod
    def from_dump(cls, raw: bytes, *, page_bytes: int) -> PhysicalNand:
        """Require a complete physical backup before interpreting target partitions."""
        expected_size = 65536 * page_bytes
        if len(raw) != expected_size:
            raise ValueError(
                f"expected a complete {expected_size}-byte physical NAND backup "
                f"(2048 main + {page_bytes - 2048} OOB bytes per page), got {len(raw)} bytes"
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


def inoi_vbm_partitions(nand: PhysicalNand) -> dict[int, tuple[int, int]]:
    """Admit the matching INOI 240/244 VBM tables and direct physical extents."""
    table = b""
    for offset in (0x7FC0000, 0x7FE0000):
        header = nand.partition_bytes((offset, 0xC2))
        if header[:8] != b"VBM_BOOT" or struct.unpack_from("<I", header, 8)[0] != 0x102:
            message = "unsupported or damaged INOI VBM header"
            raise ValueError(message)
        table = header[0x16:]
        if hashlib.sha256(table).hexdigest() != (
            "e25eb8d1ede9629d46abcfb0335d3170d5b553fad7c439e3f82f25ca14b0ab85"
        ):
            message = "unsupported or damaged INOI partition table"
            raise ValueError(message)
    count = struct.unpack_from("<H", table)[0]
    result = {}
    for index in range(count):
        identifier, _attributes, first, last = struct.unpack_from("<I3H", table, 2 + index * 10)
        result[identifier] = (first * BLOCK_MAIN_BYTES, (last - first + 1) * BLOCK_MAIN_BYTES)
    return result


def fixed_nv(partition: bytes) -> dict[int, bytes]:
    """Read the three Bluetooth records from a complete sorted NV1 stream."""
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
            for required_id, (_filename, required_size) in NV_FILES.items():
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
        if identifier in NV_FILES:
            records[identifier] = partition[cursor : cursor + length]
        cursor += padded_length
    message = "fixed NV stream has no complete terminator"
    raise ValueError(message)


def nv_checksum(data: bytes) -> int:
    """Return the NV format's little-endian one's-complement checksum."""
    total = 0
    for offset in range(0, len(data) - 1, 2):
        total += data[offset] | data[offset + 1] << 8
    if len(data) % 2:
        total += data[-1]
    total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def verify_running_nv(partition: bytes, fixed: dict[int, bytes]) -> None:
    """Require unambiguous checksum-valid copies agreeing with fixed NV.

    Physical positions do not establish which FTL record is newest. Reject
    conflicting valid values rather than choosing an occurrence by position.
    """
    for identifier, (_filename, size) in NV_FILES.items():
        signature = struct.pack("<HH", identifier, size)
        cursor = 4
        copies = 0
        while (match := partition.find(signature, cursor)) != -1:
            cursor = match + 1
            start = match - 4
            end = start + 12 + size
            if end > len(partition):
                continue
            header_checksum, data_checksum = struct.unpack_from("<HH", partition, start)
            if header_checksum != nv_checksum(partition[start + 4 : start + 12]):
                continue
            payload = partition[start + 12 : end]
            if data_checksum != nv_checksum(payload):
                raise ValueError(f"damaged RunningNV checksum for Bluetooth record {identifier}")
            if payload != fixed[identifier]:
                raise ValueError(
                    f"ambiguous Bluetooth record {identifier}: RunningNV and fixed NV disagree"
                )
            copies += 1
        if not copies:
            raise ValueError(f"no checksum-valid RunningNV copy of Bluetooth record {identifier}")


def omit_initial_pub_policy(original: bytes, offsets: tuple[int, int]) -> bytes:
    """Skip CM4's initial shared DDR policy writes while retaining its setup."""
    first, second = offsets
    if original[first : first + 2] != b"\x2d\x4c" or original[second : second + 2] != b"\xe0\x6d":
        message = "unsupported CM4 compatibility-patch instructions"
        raise ValueError(message)
    prepared = bytearray(original)
    prepared[first : first + 2] = b"\x08\xe0"
    prepared[second : second + 2] = b"\x2b\xe0"
    return bytes(prepared)


@dataclass(frozen=True)
class Cm4Revision:
    """One exact linked image with confirmed DMC_RetInit instruction locations."""

    size: int
    original_sha256: str
    prepared_sha256: str
    pub_policy_offsets: tuple[int, int]

    def prepare(self, original: bytes) -> bytes:
        """Admit the unchanged image before producing its compatible output copy."""
        if (
            len(original) != self.size
            or hashlib.sha256(original).hexdigest() != self.original_sha256
        ):
            message = "unsupported or damaged original CM4 firmware revision"
            raise ValueError(message)
        prepared = omit_initial_pub_policy(original, self.pub_policy_offsets)
        if hashlib.sha256(prepared).hexdigest() != self.prepared_sha256:
            message = "CM4 compatibility patch produced an unexpected image"
            raise ValueError(message)
        return prepared


def prepare_from_partitions(
    nand: PhysicalNand,
    partitions: dict[int, tuple[int, int]],
    *,
    prefix: str,
    revision: Cm4Revision,
) -> FirmwarePreparation:
    """Extract the common CM4/NV set from a target's admitted physical partitions."""
    cm4 = nand.partition_bytes(partitions[0x10000018])[: revision.size]
    prepared_cm4 = revision.prepare(cm4)
    fixed = fixed_nv(nand.partition_bytes(partitions[0x10000001]))
    protected = fixed_nv(nand.partition_bytes(partitions[0x1000000F]))
    if fixed != protected:
        message = "ambiguous Bluetooth settings: DownloadedNV and ProtectNV disagree"
        raise ValueError(message)
    running = nand.partition_bytes(partitions[0x10000003])
    verify_running_nv(running, fixed)

    cm4_filename = f"{prefix}-cm4.bin"
    originals = {cm4_filename: cm4}
    for identifier, (filename, _size) in NV_FILES.items():
        originals[f"{prefix}-{filename}"] = fixed[identifier]
    prepared = dict(originals)
    prepared[cm4_filename] = prepared_cm4
    return FirmwarePreparation(originals=originals, prepared=prepared)
