# SPDX-License-Identifier: GPL-2.0-only
"""Discover bounded physical partitions through a NAND image's own tables."""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING

from .formats import BLOCK_MAIN_BYTES, redundant_vbm_partitions

if TYPE_CHECKING:
    from collections.abc import Mapping

    from .formats import NandPartitionReader, RequiredPartition

_VBM_MAGIC = b"VBM_BOOT"
_PARTI_MAX_PARTITIONS = 100
_PARTI_ENTRY = struct.Struct("<4I")
_PARTI_REMAINDER = 0xFFFFFFFF
# The count precedes an extent at block zero; the second extent starts where
# the first ends. Full-table validation distinguishes these candidates from data.
_PARTI_CANDIDATE = re.compile(rb"(?s)(?=[\x02-\x64]\x00\x00\x00.{8}\x00{4}(.{4}).{8}\1)")


@dataclass(frozen=True)
class PartitionTable:
    """The admitted table format and only the extents requested by a consumer."""

    kind: str
    extents: dict[int, tuple[int, int]]


def discover_partitions(
    nand: NandPartitionReader,
    required: Mapping[int, RequiredPartition],
) -> PartitionTable:
    """Use two agreeing VBM copies, or one consistent compiled PartI table.

    VBM copies must start at physical block boundaries. Without those copies,
    a compiled PartI table may occur at any word-aligned main-area position.
    Tables describe direct physical extents; no replacement mapping is inferred.
    """
    copies = []
    for block in range(nand.block_count):
        offset = block * BLOCK_MAIN_BYTES
        if nand.main_bytes(offset, len(_VBM_MAGIC)) == _VBM_MAGIC:
            copies.append(offset)
    if copies:
        if len(copies) != 2:
            raise ValueError(f"expected two VBM partition-table copies, found {len(copies)}")
        extents = redundant_vbm_partitions(nand, (copies[0], copies[1]), required)
        return PartitionTable("VBM", extents)
    return PartitionTable("PartI", _parti_partitions(nand, required))


def _parti_partitions(
    nand: NandPartitionReader,
    required: Mapping[int, RequiredPartition],
) -> dict[int, tuple[int, int]]:
    main = nand.main_bytes(0, nand.block_count * BLOCK_MAIN_BYTES)
    tables: dict[tuple[tuple[int, int, int, int], ...], list[int]] = {}
    for candidate in _PARTI_CANDIDATE.finditer(main):
        position = candidate.start()
        if position % 4:
            continue
        entries = _parti_table(main, position, nand.block_count)
        if entries is not None:
            tables.setdefault(entries, []).append(position)
    if not tables:
        message = "no VBM or PartI partition table found"
        raise ValueError(message)
    if len(tables) != 1:
        message = "the NAND holds PartI partition tables that disagree"
        raise ValueError(message)
    (entries,) = tables
    table_bytes = 4 + len(entries) * _PARTI_ENTRY.size
    for position in tables[entries]:
        encoded = nand.partition_bytes((position, table_bytes))
        if len(encoded) != table_bytes:
            raise ValueError(f"truncated PartI partition table at {position:#x}")

    entries_by_id = {entry[0]: entry for entry in entries}
    extents = {}
    for identifier, requirement in required.items():
        entry = entries_by_id.get(identifier)
        if entry is None:
            raise ValueError(
                f"required PartI partition {requirement.name} ({identifier:#010x}) is missing"
            )
        _, attributes, first_block, blocks = entry
        if attributes != requirement.attributes:
            raise ValueError(
                f"required PartI partition {requirement.name} has attributes {attributes:#x}; "
                f"expected {requirement.attributes:#x}"
            )
        if blocks == _PARTI_REMAINDER:
            blocks = nand.block_count - first_block
        extents[identifier] = (first_block * BLOCK_MAIN_BYTES, blocks * BLOCK_MAIN_BYTES)
    return extents


def _parti_table(
    main: bytes,
    position: int,
    block_count: int,
) -> tuple[tuple[int, int, int, int], ...] | None:
    """Decode a complete, unique, contiguous table or reject this candidate."""
    count = struct.unpack_from("<I", main, position)[0]
    if not 2 <= count <= min(_PARTI_MAX_PARTITIONS, block_count):
        return None
    end = position + 4 + count * _PARTI_ENTRY.size
    if end > len(main):
        return None
    entries = tuple(_PARTI_ENTRY.iter_unpack(main[position + 4 : end]))
    expected_start = 0
    for _identifier, _attributes, start_block, blocks in entries[:-1]:
        if start_block != expected_start or blocks in (0, _PARTI_REMAINDER):
            return None
        expected_start += blocks
        if expected_start >= block_count:
            return None
    _identifier, _attributes, remainder_start, remainder_blocks = entries[-1]
    if (
        remainder_blocks != _PARTI_REMAINDER
        or remainder_start != expected_start
        or len({entry[0] for entry in entries}) != count
    ):
        return None
    return entries
