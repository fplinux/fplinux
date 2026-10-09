# SPDX-License-Identifier: GPL-2.0-only
"""Extract shared UMS9117 Bluetooth formats from physical NAND backups.

The phone's partition and load tables select its image. Individual NV values
come from the backup; this reader does not remap blocks or recover ECC.
"""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

from fplinux_cli.device_data.cm4 import cm4_image, omit_initial_pub_policy
from fplinux_cli.device_data.formats import (
    PhysicalNand,
    PreparedGroup,
    RequiredPartition,
    fixed_nv_records,
    nv_checksum,
)

if TYPE_CHECKING:
    from collections.abc import Mapping

NV_FILES = {
    401: ("bt-config.bin", 8),
    402: ("bt-sprd.bin", 176),
    404: ("bt-rf-config.bin", 252),
}

DOWNLOADED_NV_PARTITION_ID = 0x10000001
PROTECT_NV_PARTITION_ID = 0x1000000F
CM4_PARTITION_ID = 0x10000018
RUNNING_NV_PARTITION_ID = 0x10000003

REQUIRED_BLUETOOTH_PARTITIONS = {
    DOWNLOADED_NV_PARTITION_ID: RequiredPartition("DownloadedNV", 0x100),
    PROTECT_NV_PARTITION_ID: RequiredPartition("ProtectNV", 0x100),
    CM4_PARTITION_ID: RequiredPartition("CM4", 0x100),
    RUNNING_NV_PARTITION_ID: RequiredPartition("RunningNV", 0x001),
}


def fixed_nv(partition: bytes) -> dict[int, bytes]:
    """Read the three Bluetooth records from a complete sorted NV1 stream."""
    return fixed_nv_records(
        partition,
        {identifier: size for identifier, (_filename, size) in NV_FILES.items()},
    )


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


def prepare_bluetooth_from_records(
    nand: PhysicalNand,
    partitions: dict[int, tuple[int, int]],
    *,
    prefix: str,
    downloaded: Mapping[int, bytes],
    protected: Mapping[int, bytes],
) -> PreparedGroup:
    """Prepare Bluetooth from already parsed fixed NV records and the shared NAND."""
    cm4 = cm4_image(nand.partition_bytes(partitions[CM4_PARTITION_ID]))
    prepared_cm4 = omit_initial_pub_policy(cm4)
    fixed = {identifier: downloaded[identifier] for identifier in NV_FILES}
    protected_bluetooth = {identifier: protected[identifier] for identifier in NV_FILES}
    if fixed != protected_bluetooth:
        message = "ambiguous Bluetooth settings: DownloadedNV and ProtectNV disagree"
        raise ValueError(message)
    running = nand.partition_bytes(partitions[RUNNING_NV_PARTITION_ID])
    verify_running_nv(running, fixed)

    cm4_filename = f"{prefix}-cm4.bin"
    originals = {cm4_filename: cm4}
    for identifier, (filename, _size) in NV_FILES.items():
        originals[f"{prefix}-{filename}"] = fixed[identifier]
    prepared = dict(originals)
    prepared[cm4_filename] = prepared_cm4
    return PreparedGroup(originals=originals, prepared=prepared)


def prepare_bluetooth_from_partitions(
    nand: PhysicalNand,
    partitions: dict[int, tuple[int, int]],
    *,
    prefix: str,
) -> PreparedGroup:
    """Extract the common CM4/NV set from a target's admitted physical partitions."""
    downloaded = fixed_nv(nand.partition_bytes(partitions[DOWNLOADED_NV_PARTITION_ID]))
    protected = fixed_nv(nand.partition_bytes(partitions[PROTECT_NV_PARTITION_ID]))
    return prepare_bluetooth_from_records(
        nand,
        partitions,
        prefix=prefix,
        downloaded=downloaded,
        protected=protected,
    )
