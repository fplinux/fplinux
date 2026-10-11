# SPDX-License-Identifier: GPL-2.0-only
"""Small physical-page format examples for UNIWA radio preparation."""

from __future__ import annotations

import struct

from fplinux_cli.device_data.formats import PhysicalNand

from tests.small.device_data.cm4_fixtures import cm4_firmware
from tests.small.device_data.headfree_fixtures import headphone_only_headfree_records
from tests.small.device_data.nv_fixtures import nv1
from tests.small.device_data.partition_fixtures import vbm_table

_RAW_PAGE = 2112


def radio_nand(
    *, calibration_mismatch: bool = False, protected_mismatch: bool = False
) -> PhysicalNand:
    """Keep current identity before old physical copies, with agreeing logical indices.

    Main sectors are plaintext; the literal address tags carry checked BCH
    parity. This exercises format admission and selection without main ECC
    correction, a complete phone backup or physical flash access.
    """
    image = bytearray(b"\xff" * (10 * 64 * _RAW_PAGE))

    def put_main(block: int, payload: bytes) -> None:
        for offset in range(0, len(payload), 2048):
            position = (block * 64 + offset // 2048) * _RAW_PAGE
            image[position : position + min(2048, len(payload) - offset)] = payload[
                offset : offset + 2048
            ]

    def put_sector(block: int, page: int, tag: str, parity: str, payload: bytes) -> None:
        position = (block * 64 + page) * _RAW_PAGE
        image[position : position + len(payload)] = payload
        spare = bytearray(b"\xff" * 32)
        spare[1:3] = bytes.fromhex(parity)
        address = bytes.fromhex(tag)
        spare[3] = address[0]
        spare[25:28] = address[1:]
        image[position + 2048 : position + 2080] = spare

    audio, protected_audio = headphone_only_headfree_records()
    fixed = {401: b"A" * 8, 402: b"B" * 176, 404: b"C" * 252, 419: bytes(range(128))}
    protected = fixed | protected_audio
    if protected_mismatch:
        protected[404] = b"D" + b"C" * 251
    put_main(0, cm4_firmware(initial_retention=False))
    put_main(1, nv1(tuple(sorted((fixed | audio).items()))))
    put_main(2, nv1(tuple(sorted(protected.items()))))
    table = vbm_table(
        (
            (0x10000018, 0x100, 0, 0),
            (0x10000001, 0x100, 1, 1),
            (0x1000000F, 0x100, 2, 2),
            (0x10000003, 1, 3, 5),
        )
    )
    for block in (8, 9):
        put_main(block, table)
    for block in (6, 7):
        put_main(block, bytes.fromhex("5f4d41500201000000000000"))
    volume = struct.pack("<IIHHHHHHBB", 0x53544E56, 0, 401, 404, 5040, 1024, 4, 16, 1, 1)
    indices = (
        bytes.fromhex("f7ee0800001100000000000000000000")
        + bytes.fromhex("0feeb000401100000000000000000000")
        + b"\xff" * 16
        + bytes.fromhex("c3ecfc00401200000000000000000000")
    )
    for block, identity, tags, parity in (
        (
            3,
            b"current!",
            ("05000000", "05010000", "85010000", "05020000"),
            ("4f8f", "639f", "5dff", "17af"),
        ),
        (
            4,
            b"A" * 8,
            ("01000000", "01010000", "81010000", "01020000"),
            ("d8ff", "f4ef", "ca8f", "80df"),
        ),
    ):
        identity_crc = b"\x50\x88" if block == 3 else b"\xfa\xfa"
        record = b"\x65\xfe" + identity_crc + bytes.fromhex("9101080001000000") + identity
        data = bytearray(b"\xff" * 1024)
        data[256 : 256 + len(record)] = record
        calibration = b"B" * 176
        checksum = b"\x39\x39"
        if block == 3 and calibration_mismatch:
            calibration = b"D" + b"B" * 175
            checksum = b"\x37\x39"
        rf = b"\xbc\xfd" + checksum + bytes.fromhex("9201b00001000000") + calibration
        data[320 : 320 + len(rf)] = rf
        old = bytes.fromhex("65fefafa9101080001000000") + b"A" * 8
        data[512 : 512 + len(old)] = old
        configuration = bytes.fromhex("6efde4e49401fc0001000000") + b"C" * 252
        data[576 : 576 + len(configuration)] = configuration
        for page, payload in ((1, volume), (2, indices), (3, indices), (4, bytes(data))):
            put_sector(block, page, tags[page - 1], parity[page - 1], payload)
    return PhysicalNand(bytes(image), _RAW_PAGE)
