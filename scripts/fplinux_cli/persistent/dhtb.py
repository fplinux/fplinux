# SPDX-License-Identifier: GPL-2.0-only
"""DHTB boot-image containers: the unit the BootROM stages execute.

Every stage image that the UMS9117 boot chain hands to the next stage
carries a 512-byte DHTB header; t117_fdl's data_exec skips exactly
``0x200`` bytes before jumping, and the stock application partition
begins with the same magic. The header layout, as FPLinux's
``scripts/fplinux_cli/build/loaders.py`` writes it and spd_dump's
``dump_flash`` reads it back:

* ``0x00``: ``"DHTB"`` magic, one little-endian version word 1
  (``b"DHTB\\x01\\0\\0\\0"``, as ``loaders.py::_dhtb`` writes it and
  spd_dump's ``READ32_LE(buf + 4) == 1`` accepts it).
* ``0x08``: SHA-256 digest of the payload (32 bytes).
* ``0x30``: payload size, little-endian 32-bit (the top bit rejected
  by readers, as the signed check in dump_flash does).
* ``0x200``: the payload itself.

Stock images optionally continue with a 0x60-byte signature header
and a signature blob; unsigned custom images end with the payload.
"""

from __future__ import annotations

import hashlib

MAGIC = b"DHTB"
HEADER_BYTES = 0x200
VERSION = 1
SIZE_OFFSET = 0x30
DIGEST_OFFSET = 0x08


def wrap(payload: bytes) -> bytes:
    """Return the DHTB container for one stage payload."""
    if not payload or len(payload) > 0xFFFFFFFF:
        raise ValueError("stage payload must have a non-empty 32-bit size")
    header = bytearray(HEADER_BYTES)
    header[:8] = MAGIC + VERSION.to_bytes(4, "little")
    header[DIGEST_OFFSET : DIGEST_OFFSET + 32] = hashlib.sha256(payload).digest()
    struct_size = len(payload).to_bytes(4, "little")
    header[SIZE_OFFSET : SIZE_OFFSET + 4] = struct_size
    return bytes(header) + payload


class Image:
    """One parsed DHTB container with its digest and payload."""

    def __init__(self, blob: bytes) -> None:
        self.blob = blob
        if len(blob) < HEADER_BYTES or bytes(blob[:4]) != MAGIC:
            raise ValueError("image has no DHTB header")
        if int.from_bytes(blob[4:8], "little") != VERSION:
            raise ValueError("unsupported DHTB version")
        self.digest = bytes(blob[DIGEST_OFFSET : DIGEST_OFFSET + 32])
        size = int.from_bytes(blob[SIZE_OFFSET : SIZE_OFFSET + 4], "little")
        if size & 0x80000000:
            raise ValueError("unexpected DHTB signed size")
        self.payload = blob[HEADER_BYTES : HEADER_BYTES + size]
        if len(self.payload) != size:
            raise ValueError("truncated DHTB payload")
        self.size = size
        self.verified = hashlib.sha256(self.payload).digest() == self.digest

    def image_size(self) -> int:
        """Return the bytes a flash reader needs for this container."""
        return HEADER_BYTES + self.size

    @classmethod
    def parse(cls, blob: bytes) -> "Image":
        return cls(blob)


def flash_aware_extent(blob: bytes) -> int:
    """Return the DHTB image size like spd_dump's auto mode does."""
    return Image.parse(blob).image_size()

