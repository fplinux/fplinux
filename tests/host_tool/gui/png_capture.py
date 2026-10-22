# SPDX-License-Identifier: GPL-2.0-only
"""Read RGB screenshot PNGs without coupling consumers to their capture producer."""

from __future__ import annotations

import struct
import zlib
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path


def png_pixels(path: Path) -> tuple[int, int, bytes]:
    """Read the producer's unfiltered RGB PNG rows using the public file format."""
    image = path.read_bytes()
    assert image[:8] == b"\x89PNG\r\n\x1a\n"
    offset = 8
    width = height = 0
    compressed = bytearray()
    while offset < len(image):
        size, kind = struct.unpack_from(">I4s", image, offset)
        data = image[offset + 8 : offset + 8 + size]
        checksum = struct.unpack_from(">I", image, offset + 8 + size)[0]
        assert zlib.crc32(kind + data) == checksum
        if kind == b"IHDR":
            width, height, depth, colour, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", data
            )
            assert (depth, colour, compression, filtering, interlace) == (8, 2, 0, 0, 0)
        elif kind == b"IDAT":
            compressed.extend(data)
        elif kind == b"IEND":
            assert offset + 12 == len(image)
        offset += 12 + size
    rows = zlib.decompress(compressed)
    stride = 1 + width * 3
    assert len(rows) == stride * height
    pixels = bytearray()
    for row in range(height):
        assert rows[row * stride] == 0
        pixels.extend(rows[row * stride + 1 : (row + 1) * stride])
    return width, height, bytes(pixels)
