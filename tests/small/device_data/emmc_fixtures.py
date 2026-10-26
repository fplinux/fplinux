# SPDX-License-Identifier: GPL-2.0-only
"""Construct independent GPT disk fixtures around caller-owned partition contents."""

from __future__ import annotations

import shutil
import struct
import zlib
from typing import TYPE_CHECKING
from uuid import UUID

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path


def write_disk(image: Path, partitions: Sequence[tuple[str, Path]]) -> dict[str, tuple[int, int]]:
    """Write mirrored GPT records using test-owned binary layout and actual payload files."""
    entries = bytearray(128 * 128)
    extents = {}
    cursor = 2048
    for index, (name, payload) in enumerate(partitions):
        size = payload.stat().st_size
        assert size > 0
        assert size % 512 == 0
        count = size // 512
        offset = index * 128
        entries[offset : offset + 16] = UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le
        struct.pack_into("<QQQ", entries, offset + 32, cursor, cursor + count - 1, 0)
        encoded = name.encode("utf-16-le")
        entries[offset + 56 : offset + 56 + len(encoded)] = encoded
        extents[name] = (cursor * 512, size)
        cursor += count
    sectors = cursor + 2048

    def header(current: int, alternate: int, array: int) -> bytes:
        data = bytearray(512)
        struct.pack_into(
            "<8sIIIIQQQQ16sQIII",
            data,
            0,
            b"EFI PART",
            0x10000,
            92,
            0,
            0,
            current,
            alternate,
            34,
            sectors - 34,
            UUID("815cfc47-4cfe-4c9a-81de-2c9b7c433730").bytes_le,
            array,
            128,
            128,
            zlib.crc32(entries),
        )
        struct.pack_into("<I", data, 16, zlib.crc32(data[:92]))
        return bytes(data)

    with image.open("w+b") as output:
        output.truncate(sectors * 512)
        output.seek(512)
        output.write(header(1, sectors - 1, 2))
        output.seek(1024)
        output.write(entries)
        for name, payload in partitions:
            output.seek(extents[name][0])
            with payload.open("rb") as source:
                shutil.copyfileobj(source, output)
        output.seek((sectors - 33) * 512)
        output.write(entries)
        output.seek((sectors - 1) * 512)
        output.write(header(sectors - 1, 1, sectors - 33))
    return extents


def repair_header_crc(image: Path, offset: int) -> None:
    """Let a malformed-layout test retain an independently valid header checksum."""
    with image.open("r+b") as stream:
        stream.seek(offset)
        header = bytearray(stream.read(92))
        struct.pack_into("<I", header, 16, 0)
        struct.pack_into("<I", header, 16, zlib.crc32(header))
        stream.seek(offset)
        stream.write(header)


def replace_entries(image: Path, entries: bytes) -> None:
    """Keep both GPT copies and their CRCs valid around independently malformed entries."""
    sectors = image.stat().st_size // 512
    with image.open("r+b") as output:
        for array_lba, header_lba in ((2, 1), (sectors - 33, sectors - 1)):
            output.seek(array_lba * 512)
            output.write(entries)
            output.seek(header_lba * 512 + 88)
            output.write(struct.pack("<I", zlib.crc32(entries)))
    repair_header_crc(image, 512)
    repair_header_crc(image, (sectors - 1) * 512)
