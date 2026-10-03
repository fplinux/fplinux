# SPDX-License-Identifier: GPL-2.0-only
"""Independent PSF2 fixtures with solid printable glyphs and a blank space."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path


def write_solid_ascii_font(path: Path, *, width: int, height: int) -> None:
    """Write reviewable glyph rectangles, without using any shipped font data."""
    codepoints = range(32, 127)
    row_bytes = (width + 7) // 8
    row = ((1 << width) - 1) << (row_bytes * 8 - width)
    solid = row.to_bytes(row_bytes, "big") * height
    bitmap = b"".join(bytes(row_bytes * height) if code == 32 else solid for code in codepoints)
    mapping = b"".join(chr(code).encode("utf-8") + b"\xff" for code in codepoints)
    header = struct.pack("<8I", 0x864AB572, 0, 32, 1, 95, row_bytes * height, height, width)
    path.write_bytes(header + bitmap + mapping)
