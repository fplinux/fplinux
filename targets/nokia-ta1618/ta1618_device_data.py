# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Select the supported TA-1618 partition layout and fitted CM4 revision."""

from __future__ import annotations

import struct
from dataclasses import dataclass

from fplinux_cli.bluetooth_firmware import (
    REQUIRED_BLUETOOTH_PARTITIONS,
    Cm4Revision,
)
from fplinux_cli.device_data import (
    BLOCK_MAIN_BYTES,
    DeviceDataPreparation,
    NandPartitionReader,
    PhysicalNand,
)
from fplinux_cli.fitted_device_data import prepare_from_partitions

_PARTI_OFFSET = 0x63DE0
_PARTI_COUNT = 22
_PARTI_TABLE_BYTES = 356
_PARTI_ENTRY = struct.Struct("<4I")
_PARTI_ATTRIBUTES = frozenset((0x001, 0x100, 0x101))
_PARTI_REMAINDER = (0x00000008, 0x001, 0x370, 0xFFFFFFFF)

CM4 = Cm4Revision(
    size=463468,
    original_sha256="9fe3b2f3997212f6107d7f15affcc4aeccb6e08edb6ae53b9e9cd9cfba2b1eda",
    prepared_sha256="2288d4979f056b200bbcf341921730a600cd94ff3d1afb14ee1e76d505539535",
    pub_policy_offsets=(0x68C60, 0x68C7C),
)


@dataclass(frozen=True)
class _PartiPartition:
    identifier: int
    attributes: int
    start_block: int
    block_count: int


def parti_partitions(nand: NandPartitionReader) -> dict[int, tuple[int, int]]:
    """Admit the fitted compiled PartI layout without expanding its remainder marker."""
    table = nand.partition_bytes((_PARTI_OFFSET, _PARTI_TABLE_BYTES))
    if len(table) != _PARTI_TABLE_BYTES:
        message = "truncated TA-1618 PartI partition table"
        raise ValueError(message)
    count = struct.unpack_from("<I", table)[0]
    if count != _PARTI_COUNT:
        raise ValueError(f"TA-1618 PartI partition count is {count}; expected {_PARTI_COUNT}")
    entries = tuple(
        _PartiPartition(*_PARTI_ENTRY.unpack_from(table, 4 + index * _PARTI_ENTRY.size))
        for index in range(count)
    )

    entries_by_id: dict[int, _PartiPartition] = {}
    for entry in entries:
        if entry.identifier in entries_by_id:
            raise ValueError(f"duplicate TA-1618 PartI partition ID {entry.identifier:#010x}")
        if entry.attributes not in _PARTI_ATTRIBUTES:
            raise ValueError(
                f"TA-1618 PartI partition {entry.identifier:#010x} has unsupported "
                f"attributes {entry.attributes:#x}"
            )
        entries_by_id[entry.identifier] = entry

    sentinel_indices = [
        index for index, entry in enumerate(entries) if entry.block_count == 0xFFFFFFFF
    ]
    sentinel = entries[-1]
    if (
        sentinel_indices != [_PARTI_COUNT - 1]
        or (
            sentinel.identifier,
            sentinel.attributes,
            sentinel.start_block,
            sentinel.block_count,
        )
        != _PARTI_REMAINDER
    ):
        message = "TA-1618 PartI table has an invalid final remainder sentinel"
        raise ValueError(message)

    expected_start = 0
    for entry in entries[:-1]:
        if entry.block_count == 0:
            raise ValueError(
                f"TA-1618 PartI partition {entry.identifier:#010x} has a zero-sized extent"
            )
        if (
            entry.start_block >= nand.block_count
            or entry.block_count > nand.block_count - entry.start_block
        ):
            raise ValueError(
                f"TA-1618 PartI partition {entry.identifier:#010x} is outside the "
                f"{nand.block_count}-block NAND"
            )
        if entry.start_block != expected_start:
            raise ValueError(
                f"TA-1618 PartI partition {entry.identifier:#010x} starts at block "
                f"{entry.start_block}; expected contiguous block {expected_start}"
            )
        expected_start += entry.block_count
    if expected_start != sentinel.start_block:
        raise ValueError(
            f"TA-1618 PartI ordinary extents end at block {expected_start}; "
            f"remainder starts at {sentinel.start_block}"
        )

    admitted: dict[int, _PartiPartition] = {}
    for identifier, requirement in REQUIRED_BLUETOOTH_PARTITIONS.items():
        required_entry = entries_by_id.get(identifier)
        if required_entry is None:
            raise ValueError(
                f"required TA-1618 PartI partition {requirement.name} "
                f"({identifier:#010x}) is missing"
            )
        if required_entry.attributes != requirement.attributes:
            raise ValueError(
                f"required TA-1618 PartI partition {requirement.name} has attributes "
                f"{required_entry.attributes:#x}; expected {requirement.attributes:#x}"
            )
        admitted[identifier] = required_entry

    return {
        entry.identifier: (
            entry.start_block * BLOCK_MAIN_BYTES,
            entry.block_count * BLOCK_MAIN_BYTES,
        )
        for entry in admitted.values()
    }


def prepare_device_data(nand: PhysicalNand) -> DeviceDataPreparation:
    """Prepare declared groups from one NAND object and one fixed-NV parse per copy."""
    return prepare_from_partitions(
        nand,
        parti_partitions(nand),
        prefix="ta1618",
        revision=CM4,
        machine_compatible=b"nokia,ta-1618",
    )
