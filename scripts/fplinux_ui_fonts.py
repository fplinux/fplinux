#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the LVGL bitmap fonts compiled into libfplinux-ui.

The fplinux-ui APKBUILD runs this script with a resolved source manifest
beside its verified originals in the source directory:

    python3 fplinux_ui_fonts.py --tokens fplinux-ui-profile.json --sources "$srcdir" --output fonts

It writes one LVGL C font per font id that the profile table of
fplinux-ui-profile.c names (scripts/fplinux_ui_profile.py selects the slot
strings, font_id() names them), fplinux-ui-fonts.c with
fplinux_ui_builtin_font() for those ids, and fonts.log.

Steps for each font:

1. Every source face is checked against its declared checksum.
2. Kerning follows the kerning-source rule (kerning_pairs()): a GPOS 'kern'
   feature, else a legacy 'kern' table, else none. The pairs between the
   requested code points are written as one legacy 'kern' format 0 table
   and GPOS is dropped, because lv_font_conv reads kerning through
   opentype.js, which skips GPOS extension lookups and stops at the first
   lookup that covers a glyph. Outlines, metrics and hinting are kept.
3. Galmuri keeps its integer pixel grid: FauxBold adds a copy shifted right
   by one design pixel, and digits are centered in one shared advance
   without digit-to-digit kerning.
   The pinned lv_font_conv rasterizes at 4 bpp, with --autohint-off for
   Galmuri and faces that carry TrueType hinting. Text fonts take U+2713
   from Noto Sans Symbols 2. Noto Sans also takes U+2190-2193 from Noto Sans
   Math; Galmuri supplies its own arrows.
4. The C font gets the cap_height and x_height fields that lv_font_conv
   leaves at 0 (LVGL's leading-trim styles use them).
5. The C font is read back and checked: every requested code point is
   mapped, the digits have one advance, the line height is the token file's
   fonts.metrics line, and every kerning pair equals the fontTools
   reference after lv_font_conv's documented quantization. Any difference
   fails the build.

The output depends only on the sources, the token file, this script,
fontTools and lv_font_conv, which the build environment pins; lv_font_conv runs in
a scratch directory with fixed file names, so its option comment is the
same on every host.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import logging
import math
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast

import fplinux_ui_profile
from fontTools import version as fonttools_version  # type: ignore[import-untyped]
from fontTools.pens.recordingPen import DecomposingRecordingPen  # type: ignore[import-untyped]
from fontTools.pens.transformPen import TransformPen  # type: ignore[import-untyped]
from fontTools.pens.ttGlyphPen import TTGlyphPen  # type: ignore[import-untyped]
from fontTools.ttLib import TTFont, newTable  # type: ignore[import-untyped]
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0  # type: ignore[import-untyped]

if TYPE_CHECKING:
    from collections.abc import Iterable, Sequence

BPP = 4


@dataclass(frozen=True)
class Source:
    """One resolved original supplied by the source catalog."""

    url: str
    algorithm: str
    checksum: str


def source_manifest(path: Path) -> tuple[str, dict[str, Source]]:
    """Read selected pins and the converter version supplied by its npm owner."""
    metadata = json.loads(path.read_text())
    return metadata["converter_version"], {
        name: Source(record["url"], record["algorithm"], record["checksum"])
        for name, record in metadata["sources"].items()
    }


# Noto Sans faces by width and weight, as named in the token file.
NOTO_FACES = {
    ("", False): "NotoSans-Regular-2.015.ttf",
    ("", True): "NotoSans-Bold-2.015.ttf",
    ("SemiCondensed", False): "NotoSans-SemiCondensed-2.015.ttf",
    ("SemiCondensed", True): "NotoSans-SemiCondensedBold-2.015.ttf",
    ("Condensed", False): "NotoSans-Condensed-2.015.ttf",
}
SYMBOLS2 = "NotoSansSymbols2-Regular-2.008.ttf"
MATH = "NotoSansMath-Regular-3.000.ttf"


@dataclass(frozen=True)
class PixelFace:
    """One source face and its native pixel-grid preparation."""

    name: str
    size: int
    faux_bold: bool = False


GALMURI_FACES = {
    "Galmuri9": PixelFace("Galmuri9-2.40.4.ttf", 10),
    "Galmuri11 FauxBold": PixelFace("Galmuri11-2.40.4.ttf", 12, faux_bold=True),
}
PIXEL_FACES = {face.name: face for face in GALMURI_FACES.values()}


def _codes(*spans: tuple[int, int] | int) -> tuple[int, ...]:
    codes: list[int] = []
    for span in spans:
        codes.extend([span] if isinstance(span, int) else range(span[0], span[1] + 1))
    return tuple(codes)


# Text repertoire, supplemental symbols and the clock's numeric repertoire.
TEXT = _codes(
    (0x20, 0x7E),
    (0xA0, 0xFF),
    (0x400, 0x45F),
    (0x2010, 0x2027),
    0x2030,
    (0x2039, 0x203A),
    0x20AC,
    0x20BD,
    0x2116,
    0x2122,
    0x2212,
)
ARROWS = (0x2190, 0x2191, 0x2192, 0x2193)
MERGED = ((SYMBOLS2, (0x2713,)), (MATH, ARROWS))
CLOCK = tuple(sorted(ord(character) for character in " +,-.0123456789:Ee"))
DIGITS = tuple(range(0x30, 0x3A))

_SLOT = re.compile(
    r"NotoSans(?: (?P<width>Condensed|SemiCondensed))?(?P<bold> Bold)? "
    r"(?P<size>[0-9]+) (?P<bpp>[0-9]+)bpp(?P<digits> digits)?"
)
_PIXEL_SLOT = re.compile(r"(?P<family>Galmuri9|Galmuri11 FauxBold) (?P<size>[0-9]+) 4bpp")
_METRIC_SLOTS = dict(fplinux_ui_profile.FONT_SLOTS)

# GPOS value record bits: XAdvance is the only one a pair kern can carry.
_X_ADVANCE = 0x0004
# Legacy 'kern' format 0 coverage bits (Microsoft version 0 tables).
_KERN_HORIZONTAL, _KERN_MINIMUM, _KERN_CROSS_STREAM, _KERN_OVERRIDE = 1, 2, 4, 8

log = logging.getLogger("fplinux_ui_fonts")


class FontBuildError(RuntimeError):
    """A source, a kerning table or a converted font breaks a build rule."""


@dataclass(frozen=True)
class FontSpec:
    """One LVGL font: its id, source faces, size and requested code points."""

    font_id: str
    face: str
    size: int
    digits: bool
    # (face, code points) per source, the main text face first.
    ranges: tuple[tuple[str, tuple[int, ...]], ...]
    # Expected line heights from fonts.metrics, by "profile slot".
    lines: dict[str, int] = field(default_factory=dict, compare=False)

    @property
    def symbol(self) -> str:
        """Return the C name of the font's lv_font_t."""
        return "fplinux_ui_font_" + self.font_id.replace("-", "_")

    @property
    def c_name(self) -> str:
        """Return the name of the font's C file."""
        return f"fplinux-ui-font-{self.font_id}.c"

    @property
    def codes(self) -> tuple[int, ...]:
        """Return every requested code point."""
        return tuple(sorted(code for _face, codes in self.ranges for code in codes))


@dataclass
class Kerning:
    """The kerning a face gives the requested code points, by glyph name pair."""

    source: str
    pairs: dict[tuple[str, str], int]
    notes: list[str] = field(default_factory=list)


@dataclass
class ParsedFont:
    """What an LVGL C font holds, read back from lv_font_conv's output."""

    line_height: int
    base_line: int
    advances: dict[int, int]
    tops: dict[int, int]
    kern_scale: int
    # Stored 4.4 kerning values by code point pair; zero pairs are absent.
    kerning: dict[tuple[int, int], int]
    kerning_format: str


def source_path(sources: Path, name: str, originals: dict[str, Source]) -> Path:
    """Return one local original only after checking its declared native checksum."""
    pinned = originals.get(name)
    if pinned is None:
        raise FontBuildError(f"{name} is not a pinned source")
    path = sources / name
    if not path.is_file():
        raise FontBuildError(f"missing source {path} (download {pinned.url})")
    if pinned.algorithm not in {"sha256", "sha512"}:
        raise FontBuildError(f"unsupported source checksum: {pinned.algorithm}")
    with path.open("rb") as stream:
        actual = hashlib.file_digest(stream, pinned.algorithm).hexdigest()
    if actual != pinned.checksum:
        raise FontBuildError(f"{pinned.algorithm} of {path} is {actual}, pinned {pinned.checksum}")
    return path


# ------------------------------------------------------------------ plan


def font_spec(description: str) -> FontSpec:
    """Return the font of one slot string, such as "NotoSans Bold 16 4bpp"."""
    pixel = _PIXEL_SLOT.fullmatch(description)
    if pixel is not None:
        source = GALMURI_FACES[pixel["family"]]
        if int(pixel["size"]) != source.size:
            raise FontBuildError(f"{description!r}: pixel face requires size {source.size}")
        return FontSpec(
            fplinux_ui_profile.font_id("fonts", description),
            source.name,
            source.size,
            digits=False,
            ranges=((source.name, (*TEXT, *ARROWS)), (SYMBOLS2, (0x2713,))),
        )
    match = _SLOT.fullmatch(description)
    if match is None:
        message = f"unknown font {description!r}"
        raise FontBuildError(message)
    if int(match["bpp"]) != BPP:
        message = f"{description!r}: only {BPP} bpp fonts are built"
        raise FontBuildError(message)
    width, bold = match["width"] or "", bool(match["bold"])
    face = NOTO_FACES.get((width, bold))
    if face is None:
        message = f"{description!r}: no pinned Noto Sans face"
        raise FontBuildError(message)
    digits = bool(match["digits"])
    ranges = ((face, CLOCK),) if digits else ((face, TEXT), *MERGED)
    font_id = fplinux_ui_profile.font_id("fonts", description)
    return FontSpec(font_id, face, int(match["size"]), digits, ranges)


def plan(tokens: dict[str, Any]) -> list[FontSpec]:
    """Return the fonts that the profile table names, in id order."""
    fonts: dict[str, FontSpec] = {}
    for profile in fplinux_ui_profile.PROFILES:
        entry = tokens["profiles"][profile]["fonts"]
        slots = fplinux_ui_profile.font_slots(profile, entry)
        for field_name, description in slots.items():
            spec = fonts.setdefault(
                fplinux_ui_profile.font_id(profile, description), font_spec(description)
            )
            slot = _METRIC_SLOTS[field_name]
            metrics = entry["metrics"].get(slot, entry["metrics"]["body"])
            spec.lines[f"{profile} {slot}"] = int(metrics["line"])
    return [fonts[font_id] for font_id in sorted(fonts)]


# ------------------------------------------------------------------ kerning


def _is_cyrillic(code: int) -> bool:
    return 0x400 <= code <= 0x4FF


def _x_advance(record: Any) -> int:  # noqa: ANN401 -- fontTools records are untyped.
    if record is None:
        return 0
    return int(getattr(record, "XAdvance", 0) or 0)


@dataclass(frozen=True)
class _PairTable:
    """One PairPos subtable, indexed for pair lookups."""

    coverage: frozenset[str]
    # Format 1: second glyph -> X advance per covered first glyph.
    pairs: dict[str, dict[str, int]] | None
    # Format 2: class of each glyph and the X advance per class pair.
    classes1: dict[str, int]
    classes2: dict[str, int]
    values: tuple[tuple[int, ...], ...]

    def value(self, left: str, right: str) -> int | None:
        """Return the adjustment of a pair, or None if this subtable does not apply."""
        if left not in self.coverage:
            return None
        if self.pairs is not None:
            return self.pairs[left].get(right)
        return self.values[self.classes1.get(left, 0)][self.classes2.get(right, 0)]


def _pair_tables(font: Any, index: int, notes: list[str]) -> list[_PairTable]:  # noqa: ANN401
    """Return the PairPos subtables of one GPOS lookup, extensions resolved."""
    lookup = font["GPOS"].table.LookupList.Lookup[index]
    tables = []
    for subtable in lookup.SubTable:
        kind, table = lookup.LookupType, subtable
        if kind == 9:
            kind, table = subtable.ExtensionLookupType, subtable.ExtSubTable
        if kind != 2:
            notes.append(f"lookup {index} type {kind} is contextual and has no pairs")
            continue
        formats = (table.ValueFormat1 & ~_X_ADVANCE, table.ValueFormat2)
        if formats != (0, 0):
            message = (
                f"GPOS lookup {index}: value formats {table.ValueFormat1:#x}/"
                f"{table.ValueFormat2:#x} carry more than an X advance"
            )
            raise FontBuildError(message)
        coverage = list(table.Coverage.glyphs)
        if table.Format == 1:
            pairs = {
                glyph: {
                    record.SecondGlyph: _x_advance(record.Value1)
                    for record in table.PairSet[number].PairValueRecord
                }
                for number, glyph in enumerate(coverage)
            }
            tables.append(_PairTable(frozenset(coverage), pairs, {}, {}, ()))
        else:
            values = tuple(
                tuple(_x_advance(record.Value1) for record in row.Class2Record)
                for row in table.Class1Record
            )
            tables.append(
                _PairTable(
                    frozenset(coverage),
                    None,
                    dict(table.ClassDef1.classDefs),
                    dict(table.ClassDef2.classDefs),
                    values,
                )
            )
    return tables


def gpos_kerning(font: Any, codes: Sequence[int]) -> Kerning:  # noqa: ANN401
    """Evaluate the GPOS 'kern' feature between code points.

    OpenType semantics: the adjustments of every PairPos lookup of the
    feature add up; inside one lookup the first subtable that applies wins
    (format 1 when it lists the pair, format 2 when it covers the left
    glyph). Extension lookups are followed. Pairs with a Cyrillic letter use
    the 'cyrl' script, all others 'latn', else DFLT. Contextual lookups
    cannot be expressed as pairs and are recorded in the notes.
    """
    gpos = font["GPOS"].table
    features = gpos.FeatureList.FeatureRecord
    scripts = {record.ScriptTag: record.Script for record in gpos.ScriptList.ScriptRecord}
    notes: list[str] = []

    def lookups(tag: str) -> list[int]:
        script = scripts.get(tag) or scripts.get("DFLT") or scripts.get("latn")
        if script is None or script.DefaultLangSys is None:
            return []
        return sorted(
            {
                index
                for feature in script.DefaultLangSys.FeatureIndex
                if features[feature].FeatureTag == "kern"
                for index in features[feature].Feature.LookupListIndex
            }
        )

    by_script = {tag: lookups(tag) for tag in ("latn", "cyrl")}
    tables = {
        index: _pair_tables(font, index, notes)
        for index in sorted(set(by_script["latn"]) | set(by_script["cyrl"]))
    }
    cmap = font.getBestCmap()
    present = [code for code in codes if code in cmap]
    pairs: dict[tuple[str, str], int] = {}
    for left in present:
        for right in present:
            tag = "cyrl" if _is_cyrillic(left) or _is_cyrillic(right) else "latn"
            total = 0
            for index in by_script[tag]:
                for subtable in tables[index]:
                    value = subtable.value(cmap[left], cmap[right])
                    if value is not None:
                        total += value
                        break
            if total:
                pairs[cmap[left], cmap[right]] = total
    return Kerning("gpos", pairs, sorted(set(notes)))


def legacy_kerning(font: Any, codes: Sequence[int]) -> Kerning:  # noqa: ANN401
    """Read a legacy 'kern' table between code points.

    Only Microsoft version 0 tables are read. Their format 0 subtables that
    are horizontal and not cross-stream add up; an override subtable
    replaces the sum and a minimum subtable raises it to its value. Apple
    version 1 tables and other subtable formats fail the build.
    """
    table = font["kern"]
    if table.version != 0:
        message = f"Apple 'kern' table version {table.version} is not supported"
        raise FontBuildError(message)
    cmap = font.getBestCmap()
    names = {cmap[code] for code in codes if code in cmap}
    pairs: dict[tuple[str, str], int] = {}
    notes: list[str] = []
    for number, subtable in enumerate(table.kernTables):
        if subtable.format != 0:
            message = f"'kern' subtable {number} has format {subtable.format}, only 0 is read"
            raise FontBuildError(message)
        coverage = subtable.coverage
        if not coverage & _KERN_HORIZONTAL or coverage & _KERN_CROSS_STREAM:
            notes.append(f"subtable {number} is vertical or cross-stream (coverage {coverage:#x})")
            continue
        for (left, right), value in subtable.kernTable.items():
            if left not in names or right not in names:
                continue
            if coverage & _KERN_MINIMUM:
                pairs[left, right] = max(pairs.get((left, right), 0), value)
            elif coverage & _KERN_OVERRIDE:
                pairs[left, right] = value
            else:
                pairs[left, right] = pairs.get((left, right), 0) + value
    return Kerning("kern", {pair: value for pair, value in pairs.items() if value}, notes)


def has_gpos_kern(font: Any) -> bool:  # noqa: ANN401
    """Tell whether a face has a GPOS 'kern' feature."""
    return "GPOS" in font and any(
        record.FeatureTag == "kern" for record in font["GPOS"].table.FeatureList.FeatureRecord
    )


def kerning_pairs(font: Any, codes: Sequence[int]) -> Kerning:  # noqa: ANN401
    """Select the face's kerning representation.

    GPOS 'kern' first, as HarfBuzz and DirectWrite do (a legacy table is then
    ignored); else the legacy 'kern' table; else no kerning.
    """
    if has_gpos_kern(font):
        return gpos_kerning(font, codes)
    if "kern" in font:
        return legacy_kerning(font, codes)
    return Kerning("none", {})


def prepare_pixel_face(font: Any, codes: Sequence[int], pixel: PixelFace) -> None:  # noqa: ANN401
    """Keep faux-bold outlines and tabular digits on the face's whole-pixel grid."""
    step, remainder = divmod(font["head"].unitsPerEm, pixel.size)
    if remainder:
        raise FontBuildError(f"{pixel.name}: one design pixel is not an integer font unit")
    cmap = font.getBestCmap()
    digits = {cmap[code] for code in DIGITS}
    metrics = font["hmtx"].metrics
    extra = step if pixel.faux_bold else 0
    digit_advance = max(metrics[name][0] for name in digits) + extra
    glyphs = font.getGlyphSet()
    replacements: dict[str, Any] = {}
    for name in sorted({cmap[code] for code in codes}):
        advance, bearing = metrics[name]
        offset = (digit_advance - advance - extra) // (2 * step) * step if name in digits else 0
        outline = DecomposingRecordingPen(glyphs)
        glyphs[name].draw(outline)
        pen = TTGlyphPen(None)
        outline.replay(TransformPen(pen, (1, 0, 0, 1, offset, 0)))
        if pixel.faux_bold:
            outline.replay(TransformPen(pen, (1, 0, 0, 1, offset + step, 0)))
        replacements[name] = pen.glyph()
        if name in digits:
            advance = digit_advance
        elif advance:
            advance += extra
        metrics[name] = advance, bearing + offset
    for name, glyph in replacements.items():
        font["glyf"][name] = glyph
    font.recalcBBoxes = True
    for name in ("hdmx", "LTSH", "VDMX"):
        if name in font:
            del font[name]


def prepare_face(
    source: Path, codes: Sequence[int], destination: Path, *, pixel: PixelFace | None = None
) -> Kerning:
    """Write a copy of a face whose kerning is one 'kern' format 0 table.

    The table holds the kerning_pairs() between the code points; GPOS is
    dropped. The copy is read back and its table compared with the pairs.
    """
    font = TTFont(io.BytesIO(source.read_bytes()), recalcTimestamp=False, recalcBBoxes=False)
    kerning = kerning_pairs(font, codes)
    if pixel is not None:
        cmap = font.getBestCmap()
        digit_glyphs = {cmap[code] for code in DIGITS}
        # A following digit must not change a tabular digit's advance.
        kerning.pairs = {
            pair: value
            for pair, value in kerning.pairs.items()
            if not (pair[0] in digit_glyphs and pair[1] in digit_glyphs)
        }
        prepare_pixel_face(font, codes, pixel)
    if len(kerning.pairs) > 0xFFFF:
        message = f"{source.name}: {len(kerning.pairs)} pairs exceed one format 0 table"
        raise FontBuildError(message)
    table = newTable("kern")
    table.version = 0
    subtable = KernTable_format_0()
    subtable.coverage = _KERN_HORIZONTAL
    subtable.kernTable = dict(sorted(kerning.pairs.items()))
    table.kernTables = [subtable]
    font["kern"] = table
    if "GPOS" in font:
        del font["GPOS"]
    # Above about 10 900 pairs the subtable length field overflows; fontTools
    # warns and opentype.js ignores the length.
    logging.getLogger("fontTools.ttLib.tables._k_e_r_n").setLevel(logging.ERROR)
    font.save(destination)
    written = TTFont(io.BytesIO(destination.read_bytes()))
    tables = written["kern"].kernTables
    if "GPOS" in written or len(tables) != 1 or tables[0].kernTable != kerning.pairs:
        message = f"{destination.name}: the written 'kern' table differs from the reference"
        raise FontBuildError(message)
    return kerning


# ------------------------------------------------------------------ conversion


def _ranges(codes: Iterable[int]) -> str:
    spans: list[list[int]] = []
    for code in sorted(codes):
        if spans and code == spans[-1][1] + 1:
            spans[-1][1] = code
        else:
            spans.append([code, code])
    return ",".join(f"0x{a:X}" if a == b else f"0x{a:X}-0x{b:X}" for a, b in spans)


def has_hinting(path: Path) -> bool:
    """Tell whether a face carries TrueType hinting instructions."""
    with TTFont(path, lazy=True) as font:
        return "fpgm" in font


def conversion_command(
    spec: FontSpec, lv_font_conv: str, faces: dict[str, Path], autohint_off: dict[str, bool]
) -> list[str]:
    """Return the lv_font_conv command line, with file names in its directory."""
    command = [lv_font_conv]
    for face, codes in spec.ranges:
        command += ["--font", faces[face].name, "-r", _ranges(codes)]
        if autohint_off[face]:
            command.append("--autohint-off")
    return [
        *command,
        "--size",
        str(spec.size),
        "--bpp",
        str(BPP),
        "--format",
        "lvgl",
        "--no-compress",
        "--lv-include",
        "lvgl/lvgl.h",
        "--lv-font-name",
        spec.symbol,
        "-o",
        spec.c_name,
    ]


def _js_round(value: float) -> int:
    """Round as JavaScript's Math.round does: halves go up."""
    floor = math.floor(value)
    return floor + 1 if value - floor >= 0.5 else floor


_ARRAY = re.compile(
    r"static (?:LV_ATTRIBUTE_LARGE_CONST )?const \w+ (\w+)\[\] =\s*\{(.*?)\};", re.DOTALL
)
_GLYPH = re.compile(r"/\* U\+([0-9A-F]+) ")
_DSC = re.compile(
    r"\{\.bitmap_index = \d+, \.adv_w = (\d+), \.box_w = \d+, \.box_h = (\d+), "
    r"\.ofs_x = -?\d+, \.ofs_y = (-?\d+)\}"
)
_CMAP = re.compile(
    r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*"
    r"\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+), \.list_length = (\d+), "
    r"\.type = LV_FONT_FMT_TXT_CMAP_(\w+)"
)


def _field(source: str, name: str) -> int:
    match = re.search(rf"\.{name}\s*=\s*(-?\d+)", source)
    if match is None:
        message = f"the C font has no {name}"
        raise FontBuildError(message)
    return int(match.group(1))


def _numbers(body: str) -> list[int]:
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.DOTALL)
    return [int(token, 0) for token in re.findall(r"-?0x[0-9a-fA-F]+|-?[0-9]+", body)]


def _cmap(source: str, arrays: dict[str, list[int]]) -> dict[int, int]:
    """Decode the character maps into code point -> glyph id, as LVGL does."""
    glyph_ids: dict[int, int] = {}
    for fields in _CMAP.findall(source):
        start, length, first, count = (int(fields[index]) for index in (0, 1, 2, 5))
        unicode_list, offsets, kind = fields[3], fields[4], fields[6]
        if kind == "FORMAT0_TINY":
            pairs = [(start + i, first + i) for i in range(length)]
        elif kind == "FORMAT0_FULL":
            pairs = [(start + i, first + ofs) for i, ofs in enumerate(arrays[offsets])]
        elif kind == "SPARSE_TINY":
            pairs = [(start + delta, first + i) for i, delta in enumerate(arrays[unicode_list])]
        else:
            codes = arrays[unicode_list]
            pairs = [(start + codes[i], first + arrays[offsets][i]) for i in range(count)]
        glyph_ids.update(pairs)
    return glyph_ids


def parse_c_font(source: str) -> ParsedFont:
    """Read the metrics, glyphs and kerning of an lv_font_conv C font."""
    arrays = {name: _numbers(body) for name, body in _ARRAY.findall(source)}
    head, _, tail = source.partition("glyph_dsc[] = {")
    order = [int(code, 16) for code in _GLYPH.findall(head)]
    descriptors = [tuple(map(int, match)) for match in _DSC.findall(tail)]
    # Glyph id 0 is reserved; the bitmap comments list ids 1, 2, ...
    if len(descriptors) != len(order) + 1:
        message = "glyph comments and descriptors differ"
        raise FontBuildError(message)
    by_id = dict(enumerate(order, start=1))
    glyph_ids = _cmap(source, arrays)
    if {code: glyph for glyph, code in by_id.items()} != glyph_ids:
        message = "the character maps differ from the glyph order"
        raise FontBuildError(message)
    advances = {code: descriptors[glyph][0] for glyph, code in by_id.items()}
    tops = {code: descriptors[glyph][2] + descriptors[glyph][1] for glyph, code in by_id.items()}
    kerning: dict[tuple[int, int], int] = {}
    if "kern_class_values" in arrays:
        left, right = arrays["kern_left_class_mapping"], arrays["kern_right_class_mapping"]
        values, columns = arrays["kern_class_values"], _field(source, "right_class_cnt")
        for first, first_code in by_id.items():
            for second, second_code in by_id.items():
                if left[first] and right[second]:
                    value = values[(left[first] - 1) * columns + right[second] - 1]
                    if value:
                        kerning[first_code, second_code] = value
        kerning_format = "classes"
    elif "kern_pair_values" in arrays:
        ids, values = arrays["kern_pair_glyph_ids"], arrays["kern_pair_values"]
        for number, value in enumerate(values):
            if value:
                kerning[by_id[ids[2 * number]], by_id[ids[2 * number + 1]]] = value
        kerning_format = "pairs"
    else:
        kerning_format = "none"
    return ParsedFont(
        line_height=_field(source, "line_height"),
        base_line=_field(source, "base_line"),
        advances=advances,
        tops=tops,
        kern_scale=_field(source, "kern_scale"),
        kerning=kerning,
        kerning_format=kerning_format,
    )


def expected_kerning(
    spec: FontSpec, faces: dict[str, Any], kernings: dict[str, Kerning]
) -> tuple[int, dict[tuple[int, int], int]]:
    """Return lv_font_conv's kern_scale and 4.4 values for the reference pairs.

    lv_font_conv scales font units to pixels (value * size / unitsPerEm),
    takes a kerning scale of max|kern| / 7.5 rounded up to 1/16 when the
    largest value reaches 7.5 px, and stores round(px / scale * 16) in an
    int8. Pairs across two source faces are not kerned.
    """
    pixels: dict[tuple[int, int], float] = {}
    for face, codes in spec.ranges:
        font = faces[face]
        cmap, units = font.getBestCmap(), font["head"].unitsPerEm
        pairs = kernings[face].pairs
        for left in codes:
            for right in codes:
                value = pairs.get((cmap[left], cmap[right]), 0)
                if value:
                    pixels[left, right] = value * spec.size / units
    largest = max((abs(value) for value in pixels.values()), default=0.0)
    scale = math.ceil(largest / 7.5 * 16) / 16 if largest >= 7.5 else 1.0
    stored = {pair: _js_round(value / scale * 16) for pair, value in pixels.items()}
    return _js_round(scale * 16), {pair: value for pair, value in stored.items() if value}


def verify_font(
    spec: FontSpec, parsed: ParsedFont, faces: dict[str, Any], kernings: dict[str, Kerning]
) -> int:
    """Check a converted font against the build rules; return its kerned pairs."""
    missing = sorted(set(spec.codes) - set(parsed.advances))
    extra = sorted(set(parsed.advances) - set(spec.codes))
    if missing or extra:
        message = f"{spec.font_id}: missing {_ranges(missing)} extra {_ranges(extra)}"
        raise FontBuildError(message)
    digits = {parsed.advances[code] for code in DIGITS}
    if len(digits) != 1:
        message = f"{spec.font_id}: digit advances differ: {sorted(digits)}"
        raise FontBuildError(message)
    for where, line in sorted(spec.lines.items()):
        if parsed.line_height != line:
            message = f"{spec.font_id}: line height {parsed.line_height}, {where} expects {line}"
            raise FontBuildError(message)
    scale, expected = expected_kerning(spec, faces, kernings)
    if expected and parsed.kern_scale != scale:
        message = f"{spec.font_id}: kern_scale {parsed.kern_scale}, expected {scale}"
        raise FontBuildError(message)
    if parsed.kerning != expected:
        wrong = sorted(set(parsed.kerning.items()) ^ set(expected.items()))[:8]
        message = f"{spec.font_id}: kerning differs from the reference, for example {wrong}"
        raise FontBuildError(message)
    return len(expected)


_CAP_X_MARK = "/* cap_height and x_height: added by fplinux_ui_fonts.py */"

# The licence headers of the generated files are not this file's licence.
# REUSE-IgnoreStart


def finish_c_font(source: str, spec: FontSpec, parsed: ParsedFont) -> str:
    """Add the licence header and the cap and x heights to an lv_font_conv C font.

    The heights are the rasterized tops of "H" and "x", or of "E" and "e" in
    a clock font.
    """
    cap = parsed.tops[ord("E") if spec.digits else ord("H")]
    x_height = parsed.tops[ord("e") if spec.digits else ord("x")]
    anchor = re.search(r"\n    \.base_line = -?\d+,[^\n]*\n", source)
    if anchor is None or _CAP_X_MARK in source:
        message = f"{spec.font_id}: unexpected lv_font_conv output"
        raise FontBuildError(message)
    insert = f"    {_CAP_X_MARK}\n    .cap_height = {cap},\n    .x_height = {x_height},\n"
    if spec.face in PIXEL_FACES:
        attribution = (
            "Galmuri and Noto Sans Symbols 2;\n"
            " * Copyright 2019-2025 Lee Minseo; Copyright 2022 The Noto Project Authors."
        )
    else:
        attribution = "Noto Sans;\n * Copyright 2022 The Noto Project Authors."
    header = (
        "/* SPDX-License-Identifier: OFL-1.1 */\n"
        f"/* {spec.font_id}: Modified Version under the SIL Open Font License 1.1.\n"
        f" * {attribution}\n"
        " * Generated by fplinux_ui_fonts.py. Do not edit. */\n"
    )
    return header + source[: anchor.end()] + insert + source[anchor.end() :]


def resolver_source(fonts: Sequence[FontSpec]) -> str:
    """Return fplinux-ui-fonts.c: fplinux_ui_builtin_font() over the fonts."""
    lines = [
        "/* SPDX-License-Identifier: LGPL-2.1-or-later */",
        "/* Generated by fplinux_ui_fonts.py from fplinux-ui-profile.json. Do not edit. */",
        '#include "fplinux-ui-fonts.h"',
        "",
        "#include <stddef.h>",
        "#include <string.h>",
        "",
    ]
    if not fonts:
        lines += [
            "const lv_font_t *fplinux_ui_builtin_font(const char *id)",
            "{",
            "\t(void)id;",
            "\treturn NULL;",
            "}",
        ]
        return "\n".join(lines) + "\n"
    lines += [f"extern const lv_font_t {spec.symbol};" for spec in fonts]
    lines += [
        "",
        "static const struct {",
        "\tconst char *id;",
        "\tconst lv_font_t *font;",
        "} builtin_fonts[] = {",
        *(f'\t{{ "{spec.font_id}", &{spec.symbol} }},' for spec in fonts),
        "};",
        "",
        "const lv_font_t *fplinux_ui_builtin_font(const char *id)",
        "{",
        "\tsize_t i;",
        "",
        "\tif (!id)",
        "\t\treturn NULL;",
        "\tfor (i = 0; i < sizeof(builtin_fonts) / sizeof(builtin_fonts[0]); ++i)",
        "\t\tif (!strcmp(builtin_fonts[i].id, id))",
        "\t\t\treturn builtin_fonts[i].font;",
        "\treturn NULL;",
        "}",
    ]
    return "\n".join(lines) + "\n"


# REUSE-IgnoreEnd


def tool_version(lv_font_conv: str) -> str:
    """Return the version lv_font_conv reports."""
    result = subprocess.run(
        [lv_font_conv, "--version"], capture_output=True, text=True, check=True, timeout=60
    )
    return result.stdout.strip()


def build(  # noqa: PLR0913 -- every input of the build is explicit.
    fonts: Sequence[FontSpec],
    sources: Path,
    output: Path,
    lv_font_conv: str,
    *,
    check_version: bool = True,
    manifest: Path | None = None,
    report: list[str] | None = None,
) -> None:
    """Prepare the faces, convert, finish and verify each font into output."""
    expected_version, originals = source_manifest(
        manifest or sources / "fplinux-font-sources.json"
    )
    if check_version and tool_version(lv_font_conv) != expected_version:
        raise FontBuildError(f"{lv_font_conv} is not lv_font_conv {expected_version}")
    for name in originals:
        source_path(sources, name, originals)
    lines = report if report is not None else []

    def note(line: str) -> None:
        lines.append(line)
        log.info("%s", line)

    note(f"lv_font_conv {expected_version}, fontTools {fonttools_version}, {BPP} bpp")
    requested: dict[str, set[int]] = {}
    for spec in fonts:
        for face, codes in spec.ranges:
            requested.setdefault(face, set()).update(codes)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fplinux-ui-fonts-") as scratch:
        work = Path(scratch)
        prepared: dict[str, Path] = {}
        kernings: dict[str, Kerning] = {}
        autohint_off: dict[str, bool] = {}
        faces: dict[str, Any] = {}
        for face in sorted(requested):
            original = source_path(sources, face, originals)
            with TTFont(original, lazy=True) as font:
                cmap = font.getBestCmap()
            absent = sorted(requested[face] - set(cmap))
            if absent:
                message = f"{face} lacks {_ranges(absent)}"
                raise FontBuildError(message)
            prepared[face] = work / re.sub(r"-[0-9.]+\.ttf$", ".ttf", face)
            pixel = PIXEL_FACES.get(face)
            kernings[face] = prepare_face(
                original, sorted(requested[face]), prepared[face], pixel=pixel
            )
            autohint_off[face] = pixel is not None or has_hinting(original)
            faces[face] = TTFont(io.BytesIO(prepared[face].read_bytes()))
            kerning = kernings[face]
            hinting = "no hinting, light autohint"
            if autohint_off[face]:
                hinting = (
                    "pixel grid, --autohint-off" if pixel is not None else "hinted, --autohint-off"
                )
            note(
                f"face {face}: kerning source {kerning.source}, {len(kerning.pairs)} pairs"
                f"{''.join('; ' + item for item in kerning.notes)}; " + hinting
            )
        for spec in fonts:
            command = conversion_command(spec, lv_font_conv, prepared, autohint_off)
            subprocess.run(command, cwd=work, check=True, capture_output=True, timeout=300)
            text = (work / spec.c_name).read_text(encoding="utf-8")
            parsed = parse_c_font(text)
            pairs = verify_font(spec, parsed, faces, kernings)
            (output / spec.c_name).write_text(
                finish_c_font(text, spec, parsed), encoding="utf-8", newline="\n"
            )
            note(
                f"font {spec.font_id}: {len(parsed.advances)} glyphs, line {parsed.line_height}"
                f", base {parsed.base_line}, digits {parsed.advances[0x30] / 16:g} px, "
                f"kerning {parsed.kerning_format} with {pairs} pairs verified"
            )
    (output / "fplinux-ui-fonts.c").write_text(resolver_source(fonts), encoding="utf-8")
    note(f"fplinux-ui-fonts.c: {', '.join(spec.font_id for spec in fonts) or 'no fonts'}")


def main(argv: Sequence[str] | None = None) -> int:
    """Parse arguments and build the fonts."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tokens", type=Path, help="fplinux-ui-profile.json")
    parser.add_argument("--sources", type=Path, help="directory with the pinned sources")
    parser.add_argument("--output", type=Path, help="directory for the C fonts")
    parser.add_argument(
        "--source-manifest", type=Path, help="resolved font originals and converter version"
    )
    parser.add_argument("--lv-font-conv", default=shutil.which("lv_font_conv") or "lv_font_conv")
    parser.add_argument(
        "--list-sources", action="store_true", help="print each source's name, URL and checksum"
    )
    args = parser.parse_args(argv)
    if args.list_sources:
        if args.source_manifest is None and args.sources is None:
            parser.error("--source-manifest or --sources is required")
        _, originals = source_manifest(
            args.source_manifest or args.sources / "fplinux-font-sources.json"
        )
        for name, pinned in originals.items():
            print(f"{name} {pinned.url} {pinned.algorithm} {pinned.checksum}")
        return 0
    if args.tokens is None or args.sources is None or args.output is None:
        parser.error("--tokens, --sources and --output are required")
    logging.basicConfig(level=logging.INFO, format="%(message)s", stream=sys.stdout)
    tokens = cast("dict[str, Any]", json.loads(args.tokens.read_text(encoding="utf-8")))
    report: list[str] = []
    try:
        fonts = plan(tokens)
        build(
            fonts,
            args.sources,
            args.output,
            args.lv_font_conv,
            manifest=args.source_manifest,
            report=report,
        )
    except (
        FontBuildError,
        fplinux_ui_profile.ProfileError,
        subprocess.CalledProcessError,
    ) as error:
        detail = getattr(error, "stderr", None)
        print(f"fplinux_ui_fonts: {error}{f': {detail!r}' if detail else ''}", file=sys.stderr)
        return 1
    (args.output / "fonts.log").write_text("\n".join(report) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
