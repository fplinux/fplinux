#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Rasterize the FPLinux brand masters and the bootloader boot mark.

The fplinux-brand aport runs this generator with svgraster.py beside it:

    NODE_PATH=... python3 generate-brand.py --profile fplinux-ui-profile.json
        --out DIR [--boot-out DIR]

Sizes come from the logo table of each profile in fplinux-ui-profile.json:
logo.splash_mark and a "mark N" logo.about give the marks, a "lockup W wide"
logo.about gives the lockups, logo.boot_mark gives the boot marks. --out
receives transparent RGBA PNGs rendered at the exact size, for both theme
variants (light: Ink body, dark: graphite body):

    mark-light-N.png, mark-dark-N.png        N x N
    lockup-light-W.png, lockup-dark-W.png    W wide, height from the viewBox

The compositor's splash-{light,dark}-N.rgb565 marks use logo.splash_mark sizes
and the same rasters, opaque on the matching profile theme's background.
They contain tightly packed little-endian RGB565 pixels without a header.

The masters keep a transparent margin of 1/16 (sides) to 3/32 (top, bottom)
of the mark height around the penguin. A consumer keeps 1/8 of the mark
height of clear space around the mark or the lockup:
ceil(N / 8) pixels (lockup: ceil(H / 8)) free around the PNG.

--boot-out receives the bootloader's copy of the dark mark: composited opaque
on the boot background, quantized to a fixed 16-colour palette (the background
and the three mark colours plus blends on every edge between two of them),
run-length encoded per row in fplinux-boot-mark.h, with an indexed preview
boot-mark-N.png per size. The APK
does not install it.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import TYPE_CHECKING

import svgraster

if TYPE_CHECKING:
    from collections.abc import Sequence

type Rgb = tuple[int, int, int]

VARIANTS = ("light", "dark")
# Minimum readable sizes: marks 32 px high and lockups 160 px wide.
MARK_MIN_PX = 32
LOCKUP_MIN_PX = 160
# The boot screen uses the dark logo background before settings are available.
BOOT_BACKGROUND = "#202D35"
BOOT_HEADER = "fplinux-boot-mark.h"
# A run byte holds the palette index in the high nibble, length - 1 in the low.
RUN_MAX = 16
PALETTE_MAX = 16
OPAQUE = 255

_ABOUT_MARK = re.compile(r"mark ([0-9]+)")
_ABOUT_LOCKUP = re.compile(r"lockup ([0-9]+) wide")
_SHAPE = re.compile(r"<(?:path|circle)\b([^>]*)>")
_ATTRIBUTE = re.compile(r'\b(id|fill)="([^"]*)"')
# Blends on every edge between two colours of the mark: (from, to, steps,
# denominator). The outer silhouette and the breast inside the body have the
# largest colour distance and the longest edges, so they get the most steps;
# the rear foot touches the breast only on a few pixels.
_BOOT_BLENDS = (
    ("background", "body", (1, 2, 3), 4),
    ("background", "accent", (1, 2, 3), 4),
    ("body", "breast", (1, 2, 3), 4),
    ("body", "accent", (1, 2), 3),
    ("breast", "accent", (1,), 2),
)


class BrandError(Exception):
    """A profile, master or output that the generator cannot use."""


@dataclass(frozen=True)
class BootSize:
    """The boot mark size of one profile."""

    profile_width: int
    profile_height: int
    size: int


@dataclass(frozen=True)
class BrandSizes:
    """Every size the profile table asks for, ascending."""

    marks: tuple[int, ...]
    lockups: tuple[int, ...]
    boot: tuple[BootSize, ...]
    splash: tuple[int, ...]
    backgrounds: dict[str, Rgb]


@dataclass(frozen=True)
class PaletteEntry:
    """One boot palette colour: its role, exact sRGB value and panel code."""

    role: str
    rgb: Rgb

    @property
    def code(self) -> int:
        """Return the RGB565 code that fill_rect takes."""
        red, green, blue = self.rgb
        return (red >> 3) << 11 | (green >> 2) << 5 | blue >> 3

    @property
    def shown(self) -> Rgb:
        """Return the colour as the panel shows it."""
        return svgraster.rgb565(self.rgb)


@dataclass(frozen=True)
class BootMark:
    """One quantized boot mark: palette indices and their row runs."""

    size: int
    indices: bytes
    runs: bytes
    max_error: float
    mean_error: float


def _positive(container: dict[str, object], key: str, where: str) -> int:
    value = container.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise BrandError(f"{where}: {key} must be a positive integer")
    return value


def read_sizes(path: Path) -> BrandSizes:
    """Read the logo sizes of every profile; fail when one is missing."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise BrandError(f"{path}: {error}") from error
    profiles = document.get("profiles") if isinstance(document, dict) else None
    if not isinstance(profiles, dict) or not profiles:
        raise BrandError(f"{path}: no profiles")
    marks: set[int] = set()
    splash: set[int] = set()
    lockups: set[int] = set()
    boot: list[BootSize] = []
    for name, profile in profiles.items():
        where = f"{path}: profiles.{name}"
        if not isinstance(profile, dict):
            raise BrandError(f"{where}: not an object")
        logo = profile.get("logo")
        if not isinstance(logo, dict):
            raise BrandError(f"{where}: no logo")
        splash_size = _positive(logo, "splash_mark", f"{where}.logo")
        splash.add(splash_size)
        marks.add(splash_size)
        about = logo.get("about")
        mark = _ABOUT_MARK.fullmatch(about) if isinstance(about, str) else None
        lockup = _ABOUT_LOCKUP.fullmatch(about) if isinstance(about, str) else None
        if mark is not None:
            marks.add(int(mark.group(1)))
        elif lockup is not None:
            lockups.add(int(lockup.group(1)))
        else:
            raise BrandError(f'{where}.logo: about must be "mark N" or "lockup W wide"')
        boot.append(
            BootSize(
                _positive(profile, "width", where),
                _positive(profile, "height", where),
                _positive(logo, "boot_mark", f"{where}.logo"),
            )
        )
    small = sorted(size for size in marks | {item.size for item in boot} if size < MARK_MIN_PX)
    if small:
        raise BrandError(f"{path}: marks below the {MARK_MIN_PX} px brand minimum: {small}")
    narrow = sorted(width for width in lockups if width < LOCKUP_MIN_PX)
    if narrow:
        raise BrandError(f"{path}: lockups below the {LOCKUP_MIN_PX} px brand minimum: {narrow}")
    boot.sort(key=lambda item: (item.profile_width, item.profile_height))
    themes = document.get("themes")
    if not isinstance(themes, dict):
        raise BrandError(f"{path}: no themes")
    backgrounds: dict[str, Rgb] = {}
    for variant in VARIANTS:
        theme = themes.get(variant)
        background = theme.get("background") if isinstance(theme, dict) else None
        if not isinstance(background, str):
            raise BrandError(f"{path}: themes.{variant}: no background")
        try:
            backgrounds[variant] = svgraster.hex_rgb(background)
        except svgraster.RasterError as error:
            raise BrandError(f"{path}: themes.{variant}.background: {error}") from error
    return BrandSizes(
        tuple(sorted(marks)),
        tuple(sorted(lockups)),
        tuple(boot),
        tuple(sorted(splash)),
        backgrounds,
    )


def _read_master(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise BrandError(f"{path}: {error}") from error


def _write(path: Path, data: bytes) -> None:
    try:
        path.write_bytes(data)
    except OSError as error:
        raise BrandError(f"{path}: {error}") from error


def master_colours(svg: str, where: str) -> dict[str, Rgb]:
    """Return the fill of every shape with an id in a mark master."""
    fills: dict[str, Rgb] = {}
    for shape in _SHAPE.finditer(svg):
        attributes = dict(_ATTRIBUTE.findall(shape.group(1)))
        if "id" in attributes and "fill" in attributes:
            try:
                fills[attributes["id"]] = svgraster.hex_rgb(attributes["fill"])
            except svgraster.RasterError as error:
                raise BrandError(f"{where}: {attributes['id']}: {error}") from error
    return fills


def boot_palette(svg: str, where: str, background: Rgb) -> list[PaletteEntry]:
    """Build the fixed boot palette from the dark master's colours."""
    fills = master_colours(svg, where)
    try:
        base = {
            "background": background,
            "body": fills["body"],
            "breast": fills["breast"],
            "accent": fills["beak"],
        }
    except KeyError as error:
        raise BrandError(f"{where}: no shape with id {error}") from error
    for shape, colour in sorted(fills.items()):
        if colour not in base.values():
            raise BrandError(f"{where}: the boot palette lacks the {shape} colour")
    palette = [PaletteEntry(role, colour) for role, colour in base.items()]
    for start, end, steps, denominator in _BOOT_BLENDS:
        for step in steps:
            rgb = tuple(
                (first * (denominator - step) + second * step + denominator // 2) // denominator
                for first, second in zip(base[start], base[end], strict=True)
            )
            share = Fraction(step, denominator)
            role = f"{start} {1 - share} + {end} {share}"
            palette.append(PaletteEntry(role, (rgb[0], rgb[1], rgb[2])))
    if len(palette) > PALETTE_MAX or len({entry.code for entry in palette}) != len(palette):
        raise BrandError(f"{where}: the boot palette needs {PALETTE_MAX} distinct RGB565 colours")
    return palette


def quantize(raster: svgraster.Raster, palette: Sequence[PaletteEntry]) -> BootMark:
    """Composite a mark on palette[0] and map every pixel to the nearest entry."""
    background = palette[0].rgb
    indices = bytearray()
    errors: list[float] = []
    for y in range(raster.height):
        for x in range(raster.width):
            red, green, blue, alpha = raster.pixel(x, y)
            colour = [
                (value * alpha + base * (OPAQUE - alpha) + OPAQUE // 2) // OPAQUE
                for value, base in zip((red, green, blue), background, strict=True)
            ]
            distances = [
                sum((value - target) ** 2 for value, target in zip(colour, entry.rgb, strict=True))
                for entry in palette
            ]
            index = min(range(len(palette)), key=lambda item: (distances[item], item))
            indices.append(index)
            errors.append(math.sqrt(distances[index]))
    runs = bytearray()
    for y in range(raster.height):
        row = indices[y * raster.width : (y + 1) * raster.width]
        start = 0
        while start < len(row):
            length = 1
            while (
                start + length < len(row)
                and length < RUN_MAX
                and row[start + length] == row[start]
            ):
                length += 1
            runs.append(row[start] << 4 | (length - 1))
            start += length
    return BootMark(
        raster.width, bytes(indices), bytes(runs), max(errors), sum(errors) / len(errors)
    )


def splash_rgb565(raster: svgraster.Raster, background: Rgb) -> bytes:
    """Composite the mark on its theme and pack opaque little-endian RGB565."""
    pixels = bytearray()
    for y in range(raster.height):
        for x in range(raster.width):
            red, green, blue, alpha = raster.pixel(x, y)
            colour = [
                (value * alpha + base * (OPAQUE - alpha) + OPAQUE // 2) // OPAQUE
                for value, base in zip((red, green, blue), background, strict=True)
            ]
            code = (colour[0] >> 3) << 11 | (colour[1] >> 2) << 5 | colour[2] >> 3
            pixels.extend(struct.pack("<H", code))
    return bytes(pixels)


def _hex(rgb: Rgb) -> str:
    return "#{:02X}{:02X}{:02X}".format(*rgb)


def _c_bytes(data: bytes) -> list[str]:
    values = [f"0x{value:02x}," for value in data]
    return ["\t" + " ".join(values[index : index + 12]) for index in range(0, len(values), 12)]


def boot_header(
    palette: Sequence[PaletteEntry], marks: Sequence[BootMark], sizes: Sequence[BootSize]
) -> str:
    """Return the C header with the palette, the runs and the profile table."""
    lines = [
        # REUSE-IgnoreStart
        "/* SPDX-License-Identifier: CC-BY-ND-4.0 */",
        # REUSE-IgnoreEnd
        "/*",
        " * FPLinux boot mark: the dark brand mark, opaque on the boot background.",
        " * Generated by alpine/aports/fplinux-brand/generate-brand.py --boot-out;",
        " * do not edit. Each mark is size x size pixels, rows top to bottom; a row",
        " * is a sequence of run bytes, palette index in the high nibble and run",
        " * length - 1 in the low nibble. Runs never cross a row and fill it exactly.",
        " * Palette entry 0 is FPLINUX_BOOT_MARK_BACKGROUND: a screen already painted",
        " * in that colour may skip its runs.",
        " */",
        "#ifndef FPLINUX_BOOT_MARK_H",
        "#define FPLINUX_BOOT_MARK_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define FPLINUX_BOOT_MARK_BACKGROUND 0x{palette[0].code:04x}U",
        f"#define FPLINUX_BOOT_MARK_PALETTE_SIZE {len(palette)}U",
        f"#define FPLINUX_BOOT_MARK_COUNT {len(sizes)}U",
        "#define FPLINUX_BOOT_MARK_RUN_INDEX(run) ((uint8_t)(run) >> 4)",
        "#define FPLINUX_BOOT_MARK_RUN_LENGTH(run) (((uint8_t)(run) & 0x0fU) + 1U)",
        "",
        "struct fplinux_boot_mark {",
        "\tuint16_t profile_width;",
        "\tuint16_t profile_height;",
        "\tuint16_t size;",
        "\tuint16_t run_count;",
        "\tconst uint8_t *runs;",
        "};",
        "",
        "/* RGB565 codes as fill_rect takes them; comments give the sRGB source. */",
        "static const uint16_t fplinux_boot_mark_palette[] = {",
    ]
    lines += [
        f"\t0x{entry.code:04x}U, /* {index}: {_hex(entry.rgb)} {entry.role} */"
        for index, entry in enumerate(palette)
    ]
    lines += ["};"]
    for mark in marks:
        lines += [
            "",
            f"static const uint8_t fplinux_boot_mark_{mark.size}[{len(mark.runs)}] = {{",
            *_c_bytes(mark.runs),
            "};",
        ]
    lines += [
        "",
        "/*",
        " * One entry per UI profile, ascending: the screen uses the entry of the",
        " * largest profile that fits it (smaller screens use the first).",
        " */",
        "static const struct fplinux_boot_mark fplinux_boot_marks[] = {",
    ]
    by_size = {mark.size: mark for mark in marks}
    lines += [
        f"\t{{ {item.profile_width}, {item.profile_height}, {item.size}, "
        f"{len(by_size[item.size].runs)}, fplinux_boot_mark_{item.size} }},"
        for item in sizes
    ]
    lines += ["};", "", "#endif", ""]
    return "\n".join(lines)


def generate(
    masters: Path,
    sizes: BrandSizes,
    out: Path | None,
    boot_out: Path | None,
    boot_background: Rgb,
) -> list[Path]:
    """Render every requested output and return the written paths."""
    svg = {
        f"{kind}-{variant}": _read_master(masters / f"fplinux-{kind}-{variant}.svg")
        for kind in ("mark", "lockup")
        for variant in VARIANTS
    }
    assets: list[tuple[str, str, int]] = []
    if out is not None:
        for variant in VARIANTS:
            assets += [(f"mark-{variant}", f"mark-{variant}-{n}.png", n) for n in sizes.marks]
            assets += [
                (f"lockup-{variant}", f"lockup-{variant}-{w}.png", w) for w in sizes.lockups
            ]
    boot_sizes = sorted({item.size for item in sizes.boot}) if boot_out is not None else []
    jobs = sorted(
        {(master, width) for master, _name, width in assets}
        | {("mark-dark", size) for size in boot_sizes}
    )
    rasters = dict(
        zip(jobs, svgraster.render([(svg[master], width) for master, width in jobs]), strict=True)
    )
    for (master, width), raster in rasters.items():
        if master.startswith("mark-") and raster.height != width:
            raise BrandError(f"fplinux-{master}.svg is not square: {width}x{raster.height}")
    written: list[Path] = []
    if out is not None:
        out.mkdir(parents=True, exist_ok=True)
        for master, name, width in assets:
            _write(out / name, svgraster.encode_png(rasters[master, width]))
            written.append(out / name)
        for variant in VARIANTS:
            for size in sizes.splash:
                path = out / f"splash-{variant}-{size}.rgb565"
                _write(
                    path,
                    splash_rgb565(rasters[f"mark-{variant}", size], sizes.backgrounds[variant]),
                )
                written.append(path)
    if boot_out is not None:
        palette = boot_palette(svg["mark-dark"], "fplinux-mark-dark.svg", boot_background)
        marks = [quantize(rasters["mark-dark", size], palette) for size in boot_sizes]
        boot_out.mkdir(parents=True, exist_ok=True)
        shown = [(*entry.shown, OPAQUE) for entry in palette]
        for mark in marks:
            path = boot_out / f"boot-mark-{mark.size}.png"
            _write(path, svgraster.encode_indexed_png(mark.size, mark.size, shown, mark.indices))
            written.append(path)
        header = boot_out / BOOT_HEADER
        _write(header, boot_header(palette, marks, sizes.boot).encode("utf-8"))
        written.append(header)
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="generate-brand.py",
        description="Rasterize the FPLinux brand marks, lockups and boot mark.",
    )
    parser.add_argument(
        "--profile", type=Path, required=True, help="fplinux-ui-profile.json with the logo sizes"
    )
    parser.add_argument(
        "--masters",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="directory with fplinux-{mark,lockup}-{light,dark}.svg (default: script directory)",
    )
    parser.add_argument("--out", type=Path, help="directory for the mark and lockup PNGs")
    parser.add_argument("--boot-out", type=Path, help="directory for the boot mark bitmaps")
    parser.add_argument(
        "--boot-background",
        default=BOOT_BACKGROUND,
        help=f"boot screen background, #RRGGBB (default: {BOOT_BACKGROUND}, brand Ink)",
    )
    arguments = parser.parse_args(argv)
    if arguments.out is None and arguments.boot_out is None:
        parser.error("give --out, --boot-out or both")
    try:
        background = svgraster.hex_rgb(arguments.boot_background)
        sizes = read_sizes(arguments.profile)
        generate(arguments.masters, sizes, arguments.out, arguments.boot_out, background)
    except (BrandError, svgraster.RasterError) as error:
        print(f"generate-brand.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
