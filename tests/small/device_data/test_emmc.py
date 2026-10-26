# SPDX-License-Identifier: GPL-2.0-only
"""GPT admission and bounded source access without physical devices."""

from __future__ import annotations

import io
import struct
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.device_data.emmc import (
    EmmcError,
    FileSelection,
    Partition,
    PartitionReader,
    read_gpt,
    validate_file_selections,
)

from .emmc_fixtures import repair_header_crc, replace_entries, write_disk

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def disk(tmp_path: Path) -> Path:
    """Own two named GPT partitions in deliberately non-alphabetical order."""
    system = tmp_path / "system"
    system.write_bytes(b"s" * 512)
    userdata = tmp_path / "userdata"
    userdata.write_bytes(b"u" * 1024)
    image = tmp_path / "phone.bin"
    write_disk(image, (("userdata", userdata), ("system", system)))
    return image


def test_gpt_resolves_named_partitions_with_unset_entry_guids(disk: Path) -> None:
    with disk.open("rb") as source:
        partitions = read_gpt(source)
    assert partitions == {
        "userdata": Partition("userdata", 1048576, 1024),
        "system": Partition("system", 1049600, 512),
    }


@pytest.mark.parametrize("copy", ["primary", "backup"])
@pytest.mark.parametrize("part", ["header", "array"])
def test_gpt_rejects_either_corrupt_metadata_copy(disk: Path, copy: str, part: str) -> None:
    sectors = disk.stat().st_size // 512
    offset = (1 if copy == "primary" else sectors - 1) * 512
    if part == "array":
        offset = (2 if copy == "primary" else sectors - 33) * 512 + 56
    else:
        offset += 56
    with disk.open("r+b") as output:
        output.seek(offset)
        original = output.read(1)
        output.seek(offset)
        output.write(bytes([original[0] ^ 1]))
    with disk.open("rb") as source, pytest.raises(EmmcError, match=r"CRC|layouts"):
        read_gpt(source)


def test_gpt_rejects_different_disk_identity_with_valid_header_crc(disk: Path) -> None:
    offset = disk.stat().st_size - 512
    with disk.open("r+b") as output:
        output.seek(offset + 56)
        output.write(b"x" * 16)
    repair_header_crc(disk, offset)
    with disk.open("rb") as source, pytest.raises(EmmcError, match="different layouts"):
        read_gpt(source)


def test_gpt_rejects_partition_array_inside_usable_storage(disk: Path) -> None:
    with disk.open("r+b") as output:
        output.seek(512 + 72)
        output.write(struct.pack("<Q", 2048))
    repair_header_crc(disk, 512)
    with disk.open("rb") as source, pytest.raises(EmmcError, match="reserved region"):
        read_gpt(source)


@pytest.mark.parametrize("defect", ["duplicate-name", "overlap", "outside-usable-storage"])
def test_gpt_rejects_ambiguous_or_unbounded_partitions_with_valid_crcs(
    disk: Path, defect: str
) -> None:
    with disk.open("rb") as source:
        source.seek(1024)
        entries = bytearray(source.read(16384))
    if defect == "duplicate-name":
        entries[128 + 56 : 128 + 128] = "userdata".encode("utf-16-le").ljust(72, b"\0")
    elif defect == "overlap":
        struct.pack_into("<Q", entries, 128 + 32, 2049)
    else:
        struct.pack_into("<Q", entries, 32, 1)
    replace_entries(disk, bytes(entries))
    with disk.open("rb") as source, pytest.raises(EmmcError, match=r"repeats|overlap|extent"):
        read_gpt(source)


def test_partition_reader_excludes_adjacent_bytes_and_keeps_borrowed_source_open() -> None:
    source = io.BytesIO(b"before|SELECTED|after")
    with io.BufferedReader(PartitionReader(source, Partition("system", 7, 8))) as bounded:
        assert bounded.peek(3) == b"SELECTED"
        assert bounded.read(1000) == b"SELECTED"
        assert bounded.read() == b""
        assert bounded.seek(-3, io.SEEK_END) == 5
        assert bounded.read() == b"TED"
        with pytest.raises(EmmcError, match="outside"):
            bounded.seek(9)
    assert not source.closed


def test_partition_reader_rejects_truncated_source() -> None:
    with (
        io.BufferedReader(
            PartitionReader(io.BytesIO(b"short"), Partition("system", 2, 10))
        ) as bounded,
        pytest.raises(EmmcError, match="truncated"),
    ):
        bounded.read()


def test_file_selection_retains_priority_and_bounded_original_names() -> None:
    selected = validate_file_selections(
        [
            {
                "name": "audio_para",
                "locations": [
                    ["userdata", "/local/media/audio_para"],
                    ["system", "/etc/audio_para"],
                ],
                "max_bytes": 500000,
            }
        ]
    )
    assert selected == (
        FileSelection(
            "audio_para",
            (("userdata", "/local/media/audio_para"), ("system", "/etc/audio_para")),
            500000,
        ),
    )


@pytest.mark.parametrize(
    "location",
    [
        ["system", "/etc/../audio_para"],
        ["system", "etc/audio_para"],
        ["../system", "/etc/audio_para"],
    ],
)
def test_file_selection_rejects_unsafe_live_and_saved_source_paths(location: list[str]) -> None:
    with pytest.raises(EmmcError, match="unsafe"):
        validate_file_selections([{"name": "audio", "locations": [location], "max_bytes": 12}])
