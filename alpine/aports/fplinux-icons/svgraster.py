#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Rasterize SVG at exact pixel sizes with resvg and write reproducible PNGs.

The build image pins @resvg/resvg-js in /opt/quality/node-tools; an APKBUILD
sets NODE_PATH to its node_modules before it runs a generator that imports
this module. fplinux-icons and fplinux-brand share it.

Every SVG is fitted to the requested width, anti-aliased, without system
fonts, on a transparent background. resvg returns a PNG; this module decodes
it to straight RGBA and encodes the result again with fixed settings and no
ancillary chunks, so equal pixels give equal files in the pinned image.
Run directly, it renders one file: svgraster.py INPUT.svg WIDTH OUTPUT.png
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import os
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Sequence

RESVG_VERSION = "2.6.2"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
RGBA_BYTES = 4
_PNG_RGBA = 6
_PNG_INDEXED = 3
_IHDR = struct.Struct(">IIBBBBB")

# One node process renders every job: JSON in on stdin, base64 PNGs out.
_RENDER_JS = f"""
"use strict";
const fs = require("fs");
const version = require("@resvg/resvg-js/package.json").version;
if (version !== "{RESVG_VERSION}") {{
  throw new Error(`@resvg/resvg-js ${{version}}, expected {RESVG_VERSION}`);
}}
const {{ Resvg }} = require("@resvg/resvg-js");
const jobs = JSON.parse(fs.readFileSync(0, "utf8"));
const images = jobs.map((job) => {{
  const image = new Resvg(job.svg, {{
    fitTo: {{ mode: "width", value: job.width }},
    font: {{ loadSystemFonts: false }},
    shapeRendering: 2,
  }}).render();
  return Buffer.from(image.asPng()).toString("base64");
}});
process.stdout.write(JSON.stringify(images));
"""


class RasterError(Exception):
    """An SVG could not be rendered or a PNG could not be read or written."""


@dataclass(frozen=True)
class Raster:
    """Straight (not premultiplied) RGBA8 pixels, rows top to bottom."""

    width: int
    height: int
    pixels: bytes

    def __post_init__(self) -> None:
        """Reject a pixel buffer that does not match the size."""
        if self.width <= 0 or self.height <= 0:
            message = f"invalid raster size {self.width}x{self.height}"
            raise RasterError(message)
        if len(self.pixels) != self.width * self.height * RGBA_BYTES:
            message = f"raster buffer does not hold {self.width}x{self.height} RGBA pixels"
            raise RasterError(message)

    def pixel(self, x: int, y: int) -> tuple[int, int, int, int]:
        """Return the RGBA value at one coordinate."""
        offset = (y * self.width + x) * RGBA_BYTES
        red, green, blue, alpha = self.pixels[offset : offset + RGBA_BYTES]
        return red, green, blue, alpha

    def alpha(self) -> bytes:
        """Return the alpha channel, one byte per pixel."""
        return self.pixels[3::RGBA_BYTES]


def render(jobs: Sequence[tuple[str, int]]) -> list[Raster]:
    """Render (SVG text, width in pixels) jobs; heights follow each viewBox."""
    if not jobs:
        return []
    request = json.dumps([{"svg": svg, "width": width} for svg, width in jobs])
    try:
        result = subprocess.run(
            ["node", "-e", _RENDER_JS],
            input=request,
            capture_output=True,
            text=True,
            check=False,
            env=os.environ,
        )
    except OSError as error:
        message = f"cannot run node: {error}"
        raise RasterError(message) from error
    if result.returncode != 0:
        message = f"resvg failed (NODE_PATH={os.environ.get('NODE_PATH', '')}):\n{result.stderr}"
        raise RasterError(message)
    images = json.loads(result.stdout)
    if not isinstance(images, list) or len(images) != len(jobs):
        message = "resvg returned a different number of images"
        raise RasterError(message)
    rasters = [decode_png(base64.b64decode(image)) for image in images]
    for (_svg, width), raster in zip(jobs, rasters, strict=True):
        if raster.width != width:
            message = f"resvg rendered {raster.width} px wide, wanted {width}"
            raise RasterError(message)
    return rasters


def _chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    if not data.startswith(PNG_SIGNATURE):
        message = "not a PNG file"
        raise RasterError(message)
    chunks: list[tuple[bytes, bytes]] = []
    position = len(PNG_SIGNATURE)
    while position < len(data):
        if position + 8 > len(data):
            message = "truncated PNG chunk header"
            raise RasterError(message)
        (length,) = struct.unpack(">I", data[position : position + 4])
        kind = data[position + 4 : position + 8]
        body = data[position + 8 : position + 8 + length]
        if len(body) != length or position + 12 + length > len(data):
            message = "truncated PNG chunk"
            raise RasterError(message)
        (crc,) = struct.unpack(">I", data[position + 8 + length : position + 12 + length])
        if zlib.crc32(kind + body) != crc:
            message = f"PNG chunk {kind!r} has a bad CRC"
            raise RasterError(message)
        chunks.append((kind, body))
        position += 12 + length
        if kind == b"IEND":
            break
    return chunks


def _paeth(left: int, up: int, corner: int) -> int:
    estimate = left + up - corner
    to_left, to_up, to_corner = abs(estimate - left), abs(estimate - up), abs(estimate - corner)
    if to_left <= to_up and to_left <= to_corner:
        return left
    if to_up <= to_corner:
        return up
    return corner


def _unfilter(kind: int, row: bytearray, previous: bytes, step: int) -> None:
    for index, value in enumerate(row):
        left = row[index - step] if index >= step else 0
        up = previous[index]
        corner = previous[index - step] if index >= step else 0
        if kind == 0:
            predictor = 0
        elif kind == 1:
            predictor = left
        elif kind == 2:
            predictor = up
        elif kind == 3:
            predictor = (left + up) // 2
        elif kind == 4:
            predictor = _paeth(left, up, corner)
        else:
            message = f"unknown PNG filter type {kind}"
            raise RasterError(message)
        row[index] = (value + predictor) & 0xFF


def decode_png(data: bytes) -> Raster:
    """Decode an 8-bit RGBA, non-interlaced PNG, as resvg writes it."""
    chunks = _chunks(data)
    if not chunks or chunks[0][0] != b"IHDR":
        message = "PNG does not start with IHDR"
        raise RasterError(message)
    width, height, depth, colour, method, filtering, interlace = _IHDR.unpack(chunks[0][1])
    if (depth, colour, method, filtering, interlace) != (8, _PNG_RGBA, 0, 0, 0):
        message = "only 8-bit RGBA non-interlaced PNG is supported"
        raise RasterError(message)
    raw = zlib.decompress(b"".join(body for kind, body in chunks if kind == b"IDAT"))
    stride = width * RGBA_BYTES
    if len(raw) != height * (stride + 1):
        message = "PNG image data has the wrong size"
        raise RasterError(message)
    pixels = bytearray()
    previous = bytes(stride)
    for y in range(height):
        start = y * (stride + 1)
        row = bytearray(raw[start + 1 : start + 1 + stride])
        _unfilter(raw[start], row, previous, RGBA_BYTES)
        pixels += row
        previous = bytes(row)
    return Raster(width, height, bytes(pixels))


def _filtered(kind: int, row: bytes, previous: bytes, step: int) -> bytes:
    out = bytearray(len(row))
    for index, value in enumerate(row):
        left = row[index - step] if index >= step else 0
        up = previous[index]
        corner = previous[index - step] if index >= step else 0
        predictor = (0, left, up, (left + up) // 2, _paeth(left, up, corner))[kind]
        out[index] = (value - predictor) & 0xFF
    return bytes(out)


def _cost(row: bytes) -> int:
    return sum(value if value < 128 else 256 - value for value in row)


def _chunk(kind: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def _encode(header: bytes, extra: bytes, rows: list[bytes], step: int) -> bytes:
    # Per row, the filter with the smallest sum of absolute differences
    # (libpng's heuristic); deterministic for equal pixels.
    data = bytearray()
    previous = bytes(len(rows[0]))
    for row in rows:
        candidates = [_filtered(kind, row, previous, step) for kind in range(5)]
        kind = min(range(5), key=lambda index: (_cost(candidates[index]), index))
        data.append(kind)
        data += candidates[kind]
        previous = row
    return (
        PNG_SIGNATURE
        + _chunk(b"IHDR", header)
        + extra
        + _chunk(b"IDAT", zlib.compress(bytes(data), 9))
        + _chunk(b"IEND", b"")
    )


def encode_png(raster: Raster) -> bytes:
    """Encode straight RGBA8 pixels as a PNG without ancillary chunks."""
    stride = raster.width * RGBA_BYTES
    rows = [raster.pixels[y * stride : (y + 1) * stride] for y in range(raster.height)]
    header = _IHDR.pack(raster.width, raster.height, 8, _PNG_RGBA, 0, 0, 0)
    return _encode(header, b"", rows, RGBA_BYTES)


def encode_indexed_png(
    width: int, height: int, palette: Sequence[tuple[int, int, int, int]], indices: bytes
) -> bytes:
    """Encode 8-bit palette indices; palette entries are RGBA (alpha in tRNS)."""
    if not 1 <= len(palette) <= 256:
        message = "a PNG palette holds 1 to 256 colours"
        raise RasterError(message)
    if len(indices) != width * height or any(index >= len(palette) for index in indices):
        message = "palette indices do not match the image"
        raise RasterError(message)
    rows = [indices[y * width : (y + 1) * width] for y in range(height)]
    header = _IHDR.pack(width, height, 8, _PNG_INDEXED, 0, 0, 0)
    extra = _chunk(b"PLTE", bytes(channel for entry in palette for channel in entry[:3]))
    alphas = bytes(entry[3] for entry in palette).rstrip(b"\xff")
    if alphas:
        extra += _chunk(b"tRNS", alphas)
    return _encode(header, extra, rows, 1)


def hex_rgb(colour: str) -> tuple[int, int, int]:
    """Parse #RRGGBB."""
    if len(colour) != 7 or not colour.startswith("#"):
        message = f"invalid colour {colour!r}"
        raise RasterError(message)
    value = int(colour[1:], 16)
    return value >> 16, (value >> 8) & 0xFF, value & 0xFF


def rgb565(rgb: tuple[int, int, int]) -> tuple[int, int, int]:
    """Return a colour as the panel shows it: RGB565 truncation, then bit replication."""
    red, green, blue = rgb[0] >> 3, rgb[1] >> 2, rgb[2] >> 3
    return (red << 3) | (red >> 2), (green << 2) | (green >> 4), (blue << 3) | (blue >> 2)


def luminance(rgb: tuple[int, int, int]) -> float:
    """Return the WCAG 2.x relative luminance."""
    red, green, blue = rgb
    return 0.2126 * _linear(red) + 0.7152 * _linear(green) + 0.0722 * _linear(blue)


def contrast(first: tuple[int, int, int], second: tuple[int, int, int]) -> float:
    """Return the WCAG 2.x contrast ratio of two colours."""
    high, low = sorted((luminance(first), luminance(second)), reverse=True)
    return (high + 0.05) / (low + 0.05)


def _linear(value: int) -> float:
    scaled = value / 255
    return scaled / 12.92 if scaled <= 0.04045 else ((scaled + 0.055) / 1.055) ** 2.4


def cielab(rgb: tuple[int, int, int]) -> tuple[float, float, float]:
    """Return the CIELAB (D65) coordinates of an sRGB colour."""
    red, green, blue = (_linear(value) for value in rgb)
    x = (0.4124564 * red + 0.3575761 * green + 0.1804375 * blue) / 0.95047
    y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue
    z = (0.0193339 * red + 0.1191920 * green + 0.9503041 * blue) / 1.08883

    def scaled(value: float) -> float:
        return value ** (1 / 3) if value > 216 / 24389 else (24389 / 27 * value + 16) / 116

    return 116 * scaled(y) - 16, 500 * (scaled(x) - scaled(y)), 200 * (scaled(y) - scaled(z))


def ciede2000(first: tuple[float, float, float], second: tuple[float, float, float]) -> float:
    """Return the CIEDE2000 colour difference of two CIELAB colours."""
    l1, a1, b1 = first
    l2, a2, b2 = second
    chroma = (math.hypot(a1, b1) + math.hypot(a2, b2)) / 2
    g = 0.5 * (1 - math.sqrt(chroma**7 / (chroma**7 + 25**7)))
    a1p, a2p = (1 + g) * a1, (1 + g) * a2
    c1p, c2p = math.hypot(a1p, b1), math.hypot(a2p, b2)
    h1p = math.degrees(math.atan2(b1, a1p)) % 360
    h2p = math.degrees(math.atan2(b2, a2p)) % 360
    if c1p * c2p == 0:
        dhp = 0.0
    elif abs(h2p - h1p) <= 180:
        dhp = h2p - h1p
    elif h2p - h1p > 180:
        dhp = h2p - h1p - 360
    else:
        dhp = h2p - h1p + 360
    dlp, dcp = l2 - l1, c2p - c1p
    dhp_big = 2 * math.sqrt(c1p * c2p) * math.sin(math.radians(dhp / 2))
    lm, cmp_ = (l1 + l2) / 2, (c1p + c2p) / 2
    if c1p * c2p == 0:
        hm = h1p + h2p
    elif abs(h1p - h2p) <= 180:
        hm = (h1p + h2p) / 2
    elif h1p + h2p < 360:
        hm = (h1p + h2p + 360) / 2
    else:
        hm = (h1p + h2p - 360) / 2
    t = (
        1
        - 0.17 * math.cos(math.radians(hm - 30))
        + 0.24 * math.cos(math.radians(2 * hm))
        + 0.32 * math.cos(math.radians(3 * hm + 6))
        - 0.20 * math.cos(math.radians(4 * hm - 63))
    )
    rotation = 30 * math.exp(-(((hm - 275) / 25) ** 2))
    rc = 2 * math.sqrt(cmp_**7 / (cmp_**7 + 25**7))
    sl = 1 + 0.015 * (lm - 50) ** 2 / math.sqrt(20 + (lm - 50) ** 2)
    sc, sh = 1 + 0.045 * cmp_, 1 + 0.015 * cmp_ * t
    rt = -math.sin(math.radians(2 * rotation)) * rc
    return math.sqrt(
        (dlp / sl) ** 2 + (dcp / sc) ** 2 + (dhp_big / sh) ** 2 + rt * (dcp / sc) * (dhp_big / sh)
    )


def main() -> None:
    """Render one SVG file to a PNG file."""
    parser = argparse.ArgumentParser(description="Render one SVG at an exact width.")
    parser.add_argument("svg", type=Path)
    parser.add_argument("width", type=int)
    parser.add_argument("png", type=Path)
    arguments = parser.parse_args()
    try:
        (raster,) = render([(arguments.svg.read_text(encoding="utf-8"), arguments.width)])
        arguments.png.write_bytes(encode_png(raster))
    except (OSError, RasterError) as error:
        print(f"svgraster: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
