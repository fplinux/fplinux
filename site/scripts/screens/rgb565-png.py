#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Convert a packed little-endian RGB565 frame to an 8-bit RGB PNG."""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


def expand(value: int, bits: int) -> int:
    """Widen a channel to eight bits by replicating its high bits."""
    return (value << (8 - bits)) | (value >> (2 * bits - 8))


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = zlib.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def convert(width: int, height: int, source: Path, destination: Path) -> None:
    frame = source.read_bytes()
    if len(frame) != width * height * 2:
        raise SystemExit(f"{source}: expected {width * height * 2} bytes, found {len(frame)}")
    rows = bytearray()
    for (pixel,) in struct.iter_unpack("<H", frame):
        if len(rows) % (width * 3 + 1) == 0:
            rows.append(0)
        rows += bytes(
            (
                expand(pixel >> 11, 5),
                expand((pixel >> 5) & 0x3F, 6),
                expand(pixel & 0x1F, 5),
            )
        )
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    destination.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + png_chunk(b"IEND", b"")
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    arguments = parser.parse_args()
    convert(arguments.width, arguments.height, arguments.source, arguments.destination)


if __name__ == "__main__":
    main()
