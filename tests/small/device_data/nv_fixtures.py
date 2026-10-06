# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic nv input fixtures."""

from __future__ import annotations

import struct

FIXED_RECORD_SIZES = {401: 8, 402: 176, 404: 252}


def nv1(records: tuple[tuple[int, bytes], ...], generation: int = 0x12345678) -> bytes:
    """Serialize synthetic records according to the NV1 wire format."""
    parts = [struct.pack("<I", generation)]
    for identifier, payload in records:
        parts.extend(
            (
                struct.pack("<HH", identifier, len(payload)),
                payload,
                bytes(-len(payload) % 4),
            )
        )
    return b"".join(parts) + b"\xff\xff\xff\xff"


def fixed_records() -> dict[int, bytes]:
    """Use visibly synthetic values, independent of production's record registry."""
    return {401: b"A" * 8, 402: b"B" * 176, 404: b"C" * 252}


def running_nv() -> bytes:
    """Use literal checksums, independently calculated for the repeated-byte data.

    For example, eight 'A' bytes sum to 4 * 0x4141; the folded complement is
    0xfafa. No expected checksum is obtained from the production checksum code.
    """
    return b"".join(
        (
            b"\xff" * 128,
            struct.pack("<HHHHI", 0xFE65, 0xFAFA, 401, 8, 1) + b"A" * 8,
            struct.pack("<HHHHI", 0xFDBC, 0x3939, 402, 176, 1) + b"B" * 176,
            struct.pack("<HHHHI", 0xFD6E, 0xE4E4, 404, 252, 1) + b"C" * 252,
        )
    )
