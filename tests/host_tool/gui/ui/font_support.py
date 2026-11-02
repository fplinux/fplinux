# SPDX-License-Identifier: GPL-2.0-only
"""Controlled TrueType input shared by bitmap-font and widget consumers."""

from __future__ import annotations

import io
from typing import TYPE_CHECKING

from fontTools.fontBuilder import FontBuilder  # type: ignore[import-untyped]
from fontTools.pens.ttGlyphPen import TTGlyphPen  # type: ignore[import-untyped]

if TYPE_CHECKING:
    from collections.abc import Iterable


def synthetic_face(extra_codes: Iterable[int] = ()) -> bytes:
    """Create a face with 600-unit advances and 700/500-unit letter heights."""
    codes = sorted({0x20, *range(0x30, 0x3A), 0x41, 0x48, 0x56, 0x78, 0x410, *extra_codes})
    cmap = {
        code: chr(code) if chr(code).isascii() and chr(code).isalpha() else f"uni{code:04X}"
        for code in codes
    }
    glyphs = {}
    metrics = {}
    for name in [".notdef", *cmap.values()]:
        pen = TTGlyphPen(None)
        top = 500 if name in {"x", "j"} else 700
        bottom = -200 if name == "j" else 0
        if name != "uni0020":
            pen.moveTo((50, bottom))
            pen.lineTo((50, top))
            pen.lineTo((550, top))
            pen.lineTo((550, bottom))
            pen.closePath()
        glyphs[name] = pen.glyph()
        metrics[name] = (600, 50)
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(list(glyphs))
    builder.setupCharacterMap(cmap)
    builder.setupGlyf(glyphs)
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=900, descent=-300)
    builder.setupNameTable({"familyName": "FPLinux Test", "styleName": "Regular"})
    builder.setupOS2(sTypoAscender=900, sTypoDescender=-300, usWinAscent=900, usWinDescent=300)
    builder.setupPost()
    builder.addOpenTypeFeatures("languagesystem DFLT dflt; feature kern { pos A V -100; } kern;")
    stream = io.BytesIO()
    builder.save(stream)
    return stream.getvalue()
