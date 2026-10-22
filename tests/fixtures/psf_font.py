# SPDX-License-Identifier: GPL-2.0-only
"""Independent PSF2 fixtures built from test-owned bitmap rows."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Mapping
    from pathlib import Path


def write_psf_font(path: Path, *, width: int, height: int, glyphs: Mapping[int, bytes]) -> None:
    """Write test-owned MSB-first glyph rows and their Unicode mapping."""
    glyph_bytes = ((width + 7) // 8) * height
    assert glyphs
    assert all(len(bitmap) == glyph_bytes for bitmap in glyphs.values())
    header = struct.pack("<8I", 0x864AB572, 0, 32, 1, len(glyphs), glyph_bytes, height, width)
    mapping = b"".join(chr(code).encode("utf-8") + b"\xff" for code in glyphs)
    path.write_bytes(header + b"".join(glyphs.values()) + mapping)


def write_solid_ascii_font(path: Path, *, width: int, height: int) -> None:
    """Write reviewable glyph rectangles, without using any shipped font data."""
    codepoints = range(32, 127)
    row_bytes = (width + 7) // 8
    row = ((1 << width) - 1) << (row_bytes * 8 - width)
    solid = row.to_bytes(row_bytes, "big") * height
    write_psf_font(
        path,
        width=width,
        height=height,
        glyphs={code: bytes(row_bytes * height) if code == 32 else solid for code in codepoints},
    )
