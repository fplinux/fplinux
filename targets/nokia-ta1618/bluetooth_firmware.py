# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Select the supported TA-1618 partition layout and fitted CM4 revision."""

from __future__ import annotations

import hashlib
import struct

from fplinux_cli.bluetooth_firmware import (
    BLOCK_MAIN_BYTES,
    Cm4Revision,
    FirmwarePreparation,
    PhysicalNand,
    prepare_from_partitions,
)

CM4 = Cm4Revision(
    size=463468,
    original_sha256="9fe3b2f3997212f6107d7f15affcc4aeccb6e08edb6ae53b9e9cd9cfba2b1eda",
    prepared_sha256="2288d4979f056b200bbcf341921730a600cd94ff3d1afb14ee1e76d505539535",
    pub_policy_offsets=(0x68C60, 0x68C7C),
)


def _partitions(nand: PhysicalNand) -> dict[int, tuple[int, int]]:
    table = nand.partition_bytes((0x63DE0, 356))
    if hashlib.sha256(table).hexdigest() != (
        "4751b62a506777bd1590da845b30559ceac81d42ab355d5b69aafb4bbe47e4b1"
    ):
        message = "unsupported or damaged TA-1618 partition table"
        raise ValueError(message)
    count = struct.unpack_from("<I", table)[0]
    result = {}
    for index in range(count):
        identifier, _attributes, start, blocks = struct.unpack_from("<4I", table, 4 + index * 16)
        result[identifier] = (start * BLOCK_MAIN_BYTES, blocks * BLOCK_MAIN_BYTES)
    return result


def prepare_firmware(raw: bytes) -> FirmwarePreparation:
    """Prepare the complete fitted set without modifying the physical NAND backup."""
    nand = PhysicalNand.from_dump(raw, page_bytes=2176)
    return prepare_from_partitions(nand, _partitions(nand), prefix="ta1618", revision=CM4)
