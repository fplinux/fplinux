# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Prepare Konrow playback calibration from physical NAND or indexed NV."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

from fplinux_cli.device_data.audio_profile import AUDIO_RECORD_SIZES, prepare_headphone_profile
from fplinux_cli.device_data.formats import (
    DeviceDataPreparation,
    PreparedGroup,
    RequiredPartition,
    fixed_nv_records,
)
from fplinux_cli.device_data.nv_volume import read_nv_volume_records
from fplinux_cli.device_data.partitions import discover_partitions

if TYPE_CHECKING:
    from collections.abc import Mapping

    from fplinux_cli.device_data.formats import AudioProfileContext, PhysicalNand

_FIXED_NV_PARTITIONS = {
    0x10000001: RequiredPartition("DownloadedNV", 0x100),
    0x1000000F: RequiredPartition("ProtectNV", 0x100),
}
_MAP_HEADER = struct.Struct("<4sIHH")
_MAP_ENTRY = struct.Struct("<HH")


def _require_direct_extents(
    nand: PhysicalNand, copies: tuple[int, int], extents: Mapping[int, tuple[int, int]]
) -> None:
    """Require matching maps and admit only selected extents with no replacements."""
    block_bytes = nand.block_main_bytes
    first, second = (offset // block_bytes for offset in copies)
    if first == second:
        message = "Konrow VBM copies occupy the same block"
        raise ValueError(message)
    headers = [
        struct.unpack("<5H", nand.partition_bytes((offset + 0x0C, 10))) for offset in copies
    ]
    reserved, reserved_flags, peer, map_a, map_b = headers[0]
    if peer != second or headers[1] != (reserved, reserved_flags, first, map_a, map_b):
        message = "Konrow VBM copies disagree on their reserved blocks"
        raise ValueError(message)
    reserve_start = nand.block_count - reserved
    metadata_blocks = {first, second, map_a, map_b}
    if (
        reserve_start <= 0
        or len(metadata_blocks) != 4
        or any(block < reserve_start or block >= nand.block_count for block in metadata_blocks)
    ):
        message = "Konrow VBM has invalid pointers in the reserved area"
        raise ValueError(message)
    copies_of_map = []
    for block in (map_a, map_b):
        offset = block * block_bytes
        header = nand.partition_bytes((offset, _MAP_HEADER.size))
        magic, version, _, count = _MAP_HEADER.unpack(header)
        entries_size = count * _MAP_ENTRY.size
        if magic != b"_MAP" or version != 0x102 or count > reserved - len(metadata_blocks):
            message = "Konrow NAND has an invalid replacement map"
            raise ValueError(message)
        entries = nand.partition_bytes((offset + _MAP_HEADER.size, entries_size)) if count else b""
        copies_of_map.append(header + entries)
    if copies_of_map[0] != copies_of_map[1]:
        message = "Konrow NAND replacement maps disagree"
        raise ValueError(message)
    replacements = tuple(_MAP_ENTRY.iter_unpack(copies_of_map[0][_MAP_HEADER.size :]))
    sources = {source for source, _ in replacements}
    destinations = {destination for _, destination in replacements}
    if (
        len(sources) != len(replacements)
        or len(destinations) != len(replacements)
        or any(source >= reserve_start for source in sources)
        or any(
            not reserve_start <= destination < nand.block_count or destination in metadata_blocks
            for destination in destinations
        )
    ):
        message = "Konrow NAND has invalid replacement blocks"
        raise ValueError(message)
    for offset, size in extents.values():
        if offset + size > reserve_start * block_bytes or any(
            offset <= block * block_bytes < offset + size for block in sources
        ):
            message = "Konrow fixed NV depends on remapped or reserved blocks"
            raise ValueError(message)


def prepare_device_data(
    nand: PhysicalNand, *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Extract BCH-corrected playback calibration from both fixed NV copies."""
    table = discover_partitions(nand, _FIXED_NV_PARTITIONS)
    if table.vbm_copy_offsets is None:
        message = "Konrow NAND requires redundant VBM partition tables"
        raise ValueError(message)
    _require_direct_extents(nand, table.vbm_copy_offsets, table.extents)
    downloaded, protected = (
        prepare_headphone_profile(
            fixed_nv_records(nand.partition_bytes(table.extents[identifier]), AUDIO_RECORD_SIZES),
            prefix="konrow280plus",
            machine_compatible=audio.machine_compatible,
        )
        for identifier in _FIXED_NV_PARTITIONS
    )
    if downloaded.prepared != protected.prepared:
        message = "Konrow fixed NV copies disagree on headphone calibration"
        raise ValueError(message)
    return DeviceDataPreparation(groups={"audio-profile": downloaded})


def prepare_from_originals(
    originals: Mapping[str, bytes], *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Admit one complete NV volume and retain only the declared playback output."""
    if len(originals) != 1:
        message = "Konrow preparation requires one indexed NV volume"
        raise ValueError(message)
    volume = next(iter(originals.values()))
    records = read_nv_volume_records(volume, AUDIO_RECORD_SIZES)
    profile = prepare_headphone_profile(
        records,
        prefix="konrow280plus",
        machine_compatible=audio.machine_compatible,
    )
    return DeviceDataPreparation(
        groups={
            "audio-profile": PreparedGroup(
                originals=dict(originals),
                prepared=profile.prepared,
                reports=profile.reports,
            )
        }
    )
