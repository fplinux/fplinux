# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic complete indexed NV volumes with literal checksums."""

from __future__ import annotations

import struct

import pytest
from fplinux_cli.device_data.nv_volume import read_nv_volume_records


def complete_nv_volume() -> bytearray:
    """Build two indexed records; the larger record crosses 512-byte sectors.

    The first index checksum complements 600 + 2554 = 0x0c52. Its record
    header checksum complements 10 + 600 + 1 = 0x0263, and zero payload words
    checksum to ffff. The eight 'A' bytes in the second record checksum to fafa.
    """
    volume = bytearray(b"\xff" * 4096)
    header = struct.pack("<IIHHHHHHBB", 0x53544E56, 0, 10, 11, 8, 512, 2, 16, 1, 1)
    header_sector = header + bytes(512 - len(header))
    volume[:512] = header_sector
    volume[512:1024] = header_sector
    index = (
        bytes.fromhex("adf35802fa0900000000000000000000")
        + bytes.fromhex("77f30800800c00000000000000000000")
        + bytes(480)
    )
    volume[1024:1536] = index
    volume[1536:2048] = index
    volume[2554:3166] = bytes.fromhex("9cfdffff0a00580201000000") + bytes(600)
    volume[3200:3220] = bytes.fromhex("ebfffafa0b00080001000000") + b"A" * 8
    return volume


def test_complete_volume_selects_exact_records_across_sector_boundaries() -> None:
    volume = complete_nv_volume()

    assert read_nv_volume_records(bytes(volume), {10: 600, 11: 8}) == {
        10: bytes(600),
        11: b"A" * 8,
    }


def test_complete_volume_returns_only_required_records() -> None:
    volume = complete_nv_volume()
    volume[3212] ^= 1

    assert read_nv_volume_records(bytes(volume), {10: 600}) == {10: bytes(600)}


@pytest.mark.parametrize(
    ("offsets", "error"),
    [
        pytest.param((1024, 1536), "invalid current RunningNV index entry", id="index"),
        pytest.param((2554,), "invalid selected RunningNV record", id="record-header"),
        pytest.param((3072,), "invalid selected RunningNV record", id="record-payload"),
    ],
)
def test_selected_record_rejects_invalid_checksums(offsets: tuple[int, ...], error: str) -> None:
    volume = complete_nv_volume()
    for offset in offsets:
        volume[offset] ^= 1

    with pytest.raises(ValueError, match=error):
        read_nv_volume_records(bytes(volume), {10: 600})


@pytest.mark.parametrize(
    ("offset", "error"),
    [
        pytest.param(512 + 26, "header copies disagree", id="header"),
        pytest.param(1536 + 16, "index copies disagree", id="unselected-index"),
    ],
)
def test_complete_volume_rejects_disagreeing_redundant_copies(offset: int, error: str) -> None:
    volume = complete_nv_volume()
    volume[offset] ^= 1

    with pytest.raises(ValueError, match=error):
        read_nv_volume_records(bytes(volume), {10: 600})


def test_required_record_must_be_present_in_the_index() -> None:
    volume = complete_nv_volume()
    missing = bytes.fromhex("ffff0000000000000000000000000000")
    volume[1040:1056] = missing
    volume[1552:1568] = missing

    with pytest.raises(ValueError, match="invalid current RunningNV index entry"):
        read_nv_volume_records(bytes(volume), {11: 8})


def test_required_record_must_have_the_exact_size() -> None:
    with pytest.raises(ValueError, match="invalid current RunningNV index entry"):
        read_nv_volume_records(bytes(complete_nv_volume()), {10: 601})


@pytest.mark.parametrize("extra_sector", [False, True], ids=["truncated", "extra"])
def test_complete_volume_size_must_match_the_declared_sector_count(*, extra_sector: bool) -> None:
    complete = bytes(complete_nv_volume())
    volume = complete + bytes(512) if extra_sector else complete[:-512]

    with pytest.raises(ValueError, match="size does not match its declared sector count"):
        read_nv_volume_records(volume, {10: 600})
