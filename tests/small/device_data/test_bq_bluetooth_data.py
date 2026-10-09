# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic format examples; main ECC and physical flash are outside this boundary."""

from __future__ import annotations

import importlib.util
import struct
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.common import ROOT
from fplinux_cli.device_data.bq2830l_radio_nv import read_running_nv_record
from fplinux_cli.device_data.formats import PhysicalNand

if TYPE_CHECKING:
    from types import ModuleType


_RAW_PAGE = 2176
_PAGES = 64
_MAIN = 2048


def put_sector(  # noqa: PLR0913 -- location, tag, parity and contents are distinct fixture inputs.
    image: bytearray,
    block: int,
    page: int,
    *,
    tag: bytes,
    parity: bytes,
    contents: bytes,
) -> None:
    """Store a literal address codeword beside a plaintext fake main sector."""
    start = (block * _PAGES + page) * _RAW_PAGE
    image[start : start + len(contents)] = contents
    spare = bytearray(b"\xff" * 32)
    spare[1:3] = parity
    spare[3] = tag[0]
    spare[25:28] = tag[1:]
    image[start + _MAIN : start + _MAIN + 32] = spare


def current_record_image() -> bytearray:
    """Build an image where generation5 precedes generation1 and indices agree.

    Tag parity is literal BCH(63,51) with generator0x1539. The record header
    checksum is fe65; current/former payload checksums are 8850/8fa0.
    Index1100/1200 checksums are eef7/edf7. No producer supplies these literals.
    """
    image = bytearray(b"\xff" * (7 * _PAGES * _RAW_PAGE))
    for block in (3, 4):
        start = block * _PAGES * _RAW_PAGE
        image[start : start + 12] = bytes.fromhex("5f4d41500201000000000000")
    volume = bytes.fromhex("564e54530000080191019101fc000004010010000101")
    index = bytes.fromhex("f7ee0800001100000000000000000000")
    old_record = bytes.fromhex("65fea08f9101080001000000") + b"former!!"
    new_record = bytes.fromhex("65fe50889101080001000000") + b"current!"
    for block, records in ((0, new_record), (1, old_record)):
        if block == 0:
            tags = ("05000000", "05010000", "85010000", "05020000")
            parity = ("4f8f", "639f", "5dff", "17af")
        else:
            tags = ("01000000", "01010000", "81010000", "01020000")
            parity = ("d8ff", "f4ef", "ca8f", "80df")
        put_sector(
            image,
            block,
            1,
            tag=bytes.fromhex(tags[0]),
            parity=bytes.fromhex(parity[0]),
            contents=volume,
        )
        put_sector(
            image,
            block,
            2,
            tag=bytes.fromhex(tags[1]),
            parity=bytes.fromhex(parity[1]),
            contents=index,
        )
        put_sector(
            image,
            block,
            3,
            tag=bytes.fromhex(tags[2]),
            parity=bytes.fromhex(parity[2]),
            contents=index,
        )
        main = bytearray(b"\xff" * 1024)
        main[256 : 256 + len(records)] = records
        main[512 : 512 + len(old_record)] = old_record
        put_sector(
            image,
            block,
            4,
            tag=bytes.fromhex(tags[3]),
            parity=bytes.fromhex(parity[3]),
            contents=bytes(main),
        )
    return image


def read_current(image: bytearray) -> bytes:
    nand = PhysicalNand(bytes(image), _RAW_PAGE)
    return read_running_nv_record(nand, (0, 3 * 131072), identifier=401, size=8, virtual_blocks=2)


def test_logical_indices_select_current_value_before_older_physical_records() -> None:
    assert read_current(current_record_image()) == b"current!"


@pytest.mark.parametrize("errors", [(3,), (2,), (3, 25), (3, 2)])
def test_literal_address_tags_correct_one_or_two_bits(errors: tuple[int, ...]) -> None:
    image = current_record_image()
    spare = _RAW_PAGE + _MAIN
    for offset in errors:
        image[spare + offset] ^= 0x10

    assert read_current(image) == b"current!"


def test_valid_current_indices_pointing_to_distinct_records_are_rejected() -> None:
    image = current_record_image()
    backup = 3 * _RAW_PAGE
    image[backup : backup + 16] = bytes.fromhex("f7ed0800001200000000000000000000")

    with pytest.raises(ValueError, match="indices disagree"):
        read_current(image)


def test_damaged_current_record_is_not_replaced_by_historical_value() -> None:
    image = current_record_image()
    image[4 * _RAW_PAGE + 256 + 12] ^= 1

    with pytest.raises(ValueError, match="selected RunningNV record"):
        read_current(image)


def test_nonempty_lower_remapping_is_rejected_before_logical_selection() -> None:
    image = current_record_image()
    struct.pack_into("<H", image, 3 * _PAGES * _RAW_PAGE + 10, 1)

    with pytest.raises(ValueError, match="unsupported NAND remapping"):
        read_current(image)


@pytest.fixture
def radio_parser() -> ModuleType:
    """Load the target parser normally without any fitted input bytes."""
    path = ROOT / "targets/bq-2830l-tank-connect-max/bq2830l_device_data.py"
    spec = importlib.util.spec_from_file_location("bq_bluetooth_parser", path)
    if spec is None or spec.loader is None:
        message = "target parser source is unavailable"
        raise ImportError(message)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_selected_address_and_controller_fields_have_literal_wire_bytes(
    radio_parser: ModuleType,
) -> None:
    configuration = bytearray(bytes(range(256)) * 3 + bytes(range(120)))
    configuration[0] = 2
    settings = bytes.fromhex("01 02 03 04 05 06 a5 5a")
    expected = bytes.fromhex(
        "6c6d6e6f707172737475767778797a7b"
        "7c7d7e7f808182838485868788898a8b"
        "8c8d8e8f909192939495969798999a9b"
        "9c9d9e9fa0a1a2a3a4a5a6a7"
        "101112131415161718191a1b1c1d1e1f"
        "010203040506262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f"
        "404142434445464748494a4b4c4d4e4f"
        "5051525354555657"
        "a8a9aaabacadaeafb0b1b2b3b4b5b6b7"
        "b8b9babbbcbdbebfc0c1c2c3c4c5c6c7"
        "c8c9cacbcccdcecfd0d1d2d3d4d5d6d7"
        "d8d9dadbdcdddedfe0e1e2e3e4e5e6e7"
        "e8e9eaeb"
        "ecedf0f1f4f5f8f9fcfd000104050809"
        "00000000"
    )

    assert radio_parser.prepare_bluetooth_pskey(bytes(configuration), settings) == expected


@pytest.mark.parametrize("settings", [bytes(7), bytes(9), bytes(8)])
def test_missing_or_zero_controller_identity_is_rejected(
    radio_parser: ModuleType, settings: bytes
) -> None:
    configuration = bytearray(888)
    configuration[0] = 2

    with pytest.raises(
        ValueError, match=r"saved Bluetooth (record has the wrong size|address is zero)"
    ):
        radio_parser.prepare_bluetooth_pskey(bytes(configuration), settings)


@pytest.mark.parametrize("configuration", [bytes(887), bytes(888), bytes([3]) + bytes(887)])
def test_unknown_configuration_layout_is_rejected(
    radio_parser: ModuleType, configuration: bytes
) -> None:
    with pytest.raises(ValueError, match="unsupported external Bluetooth radio configuration"):
        radio_parser.prepare_bluetooth_pskey(
            configuration, bytes.fromhex("01 02 03 04 05 06 00 00")
        )
