#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
r"""Rasterize the FPLinux system glyphs and application tiles.

The fplinux-icons APKBUILD runs, in its source directory,

    NODE_PATH=/opt/quality/node-tools/node_modules python3 generate-icons.py \
        --tabler package/icons --out ROOT [--report FILE]

icons.json names every glyph and tile; fplinux-ui-profile.json
gives the tile colours, tile radii, selection geometry and the
page colours of both themes. Outputs below ROOT:

    usr/share/fplinux/glyphs/<N>/<name>.png         N = glyph_sizes
    usr/share/icons/hicolor/<N>x<N>/apps/<id>.png   N = profile tile_radius keys

Glyphs are alpha masks (black RGBA, the runtime keeps only alpha); tiles are
opaque colour squares with rounded corners and a white glyph. resvg draws the
glyph from tile.svg; the rounded square's coverage is computed here on a
symmetric 16 x 16 sample grid, because resvg's anti-aliasing differs by up to
a quarter of full coverage between mirrored corner pixels, which would put a
tile off-centre in its concentric selection ring.

The run fails when a design rule does not hold: the filled/outline rule per
size (unless the manifest gives a glyph an exception with its reason), the
tile radius range, integer glyph placement, the WCAG contrast
after RGB565, the CIEDE2000 distance between every two tile colours, the
selection geometry and the symmetry of the tile shape.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
import re
import string
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import svgraster
from svgraster import Raster

HERE = Path(__file__).resolve().parent
WHITE = (255, 255, 255)
_COMMENT = re.compile(r"<!--.*?-->", re.DOTALL)
_SVG_OPEN = re.compile(r"<svg\b[^>]*>")
_STROKE_WIDTH = re.compile(r'stroke-width="[^"]*"')


class IconError(Exception):
    """A manifest entry, a source file or a design rule is wrong."""


@dataclass(frozen=True)
class Output:
    """One PNG to write: path below the output root and its pixels."""

    path: str
    raster: Raster
    record: dict[str, Any]


def _load_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        message = f"cannot read {path}: {error}"
        raise IconError(message) from error
    if not isinstance(data, dict):
        message = f"{path} is not a JSON object"
        raise IconError(message)
    return data


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


SUBSAMPLES = 16


def rounded_square_alpha(size: int, radius: int) -> bytes:
    """Return the coverage of a size x size square with corner radius radius.

    Each pixel counts SUBSAMPLES x SUBSAMPLES points at odd multiples of
    1/(2 SUBSAMPLES) px; the grid is symmetric, so the mask is symmetric under
    both mirrors and the diagonal, and its centre is the square's centre.
    """
    unit = 2 * SUBSAMPLES
    centre = radius * unit
    alpha = bytearray(b"\xff" * size * size)
    for corner_y in range(radius):
        for corner_x in range(radius):
            inside = 0
            for sample_y in range(SUBSAMPLES):
                dy = centre - (corner_y * unit + 2 * sample_y + 1)
                for sample_x in range(SUBSAMPLES):
                    dx = centre - (corner_x * unit + 2 * sample_x + 1)
                    if dx <= 0 or dy <= 0 or dx * dx + dy * dy <= centre * centre:
                        inside += 1
            value = (inside * 255 + SUBSAMPLES * SUBSAMPLES // 2) // (SUBSAMPLES * SUBSAMPLES)
            for y, x in (
                (corner_y, corner_x),
                (corner_y, size - 1 - corner_x),
                (size - 1 - corner_y, corner_x),
                (size - 1 - corner_y, size - 1 - corner_x),
            ):
                alpha[y * size + x] = value
    return bytes(alpha)


def _svg_body(svg: str) -> str:
    """Return the children of the root svg element, without comments."""
    svg = _COMMENT.sub("", svg)
    opening = _SVG_OPEN.search(svg)
    closing = svg.rfind("</svg>")
    if opening is None or closing < opening.end():
        message = "not an SVG document"
        raise IconError(message)
    return svg[opening.end() : closing].strip()


class Sources:
    """Glyph SVG files: Tabler's outline/ and filled/ trees and project files."""

    def __init__(self, tabler: Path, project: Path) -> None:
        """Remember the two source roots."""
        self.tabler = tabler
        self.project = project
        self.used: dict[str, str] = {}

    def path(self, source: str, name: str, style: str) -> Path:
        """Return the SVG file of one glyph style (it may not exist)."""
        if source == "tabler":
            return self.tabler / style / f"{name}.svg"
        if source == "project":
            suffix = "-filled" if style == "filled" else ""
            return self.project / f"{name}{suffix}.svg"
        message = f"unknown glyph source {source!r}"
        raise IconError(message)

    def read(self, source: str, name: str, style: str) -> str:
        """Read one glyph SVG and record its digest for the report."""
        path = self.path(source, name, style)
        try:
            data = path.read_bytes()
        except OSError as error:
            message = f"missing {source} {style} glyph {name!r}: {path}"
            raise IconError(message) from error
        label = f"{source}:{style}/{name}.svg"
        self.used[label] = _sha256(data)
        return data.decode("utf-8")


def _sizes(manifest: dict[str, Any], key: str) -> list[int]:
    sizes = manifest.get(key)
    if (
        not isinstance(sizes, list)
        or not sizes
        or not all(isinstance(size, int) and size > 0 for size in sizes)
    ):
        message = f"icons.json {key} must be a list of positive integers"
        raise IconError(message)
    return sizes


def plan_glyphs(
    manifest: dict[str, Any], sources: Sources
) -> list[tuple[str, str, int, dict[str, Any]]]:
    """Return (svg, output path, size, record) for every glyph at every size."""
    sizes = _sizes(manifest, "glyph_sizes")
    filled_max = manifest.get("filled_max_size")
    if not isinstance(filled_max, int):
        message = "icons.json filled_max_size must be an integer"
        raise IconError(message)
    glyphs = manifest.get("glyphs")
    if not isinstance(glyphs, dict) or not glyphs:
        message = "icons.json glyphs must be a non-empty object"
        raise IconError(message)
    jobs: list[tuple[str, str, int, dict[str, Any]]] = []
    for name, entry in sorted(glyphs.items()):
        source, upstream, styles = entry.get("source"), entry.get("name"), entry.get("styles")
        if not isinstance(source, str) or not isinstance(upstream, str):
            message = f"glyph {name!r} needs a source and a name"
            raise IconError(message)
        if not isinstance(styles, dict) or sorted(styles) != sorted(str(size) for size in sizes):
            message = f"glyph {name!r} needs a style for each of {sizes}"
            raise IconError(message)
        exception, stroke_px = entry.get("exception"), entry.get("stroke_px")
        if exception is not None and not (isinstance(exception, str) and exception):
            message = f"glyph {name!r}: exception must give its reason"
            raise IconError(message)
        if stroke_px is not None and not isinstance(stroke_px, int | float):
            message = f"glyph {name!r}: stroke_px must be a number"
            raise IconError(message)
        has_filled = sources.path(source, upstream, "filled").is_file()
        for size in sizes:
            style = styles[str(size)]
            expected = "filled" if size <= filled_max and has_filled else "outline"
            if style != expected and exception is None:
                message = (
                    f"glyph {name!r} at {size} px is {style!r}; the rule gives {expected!r} "
                    f"(filled variant {'present' if has_filled else 'absent'})"
                )
                raise IconError(message)
            svg = sources.read(source, upstream, style)
            record: dict[str, Any] = {
                "glyph": name,
                "size": size,
                "style": style,
                "source": f"{source}:{style}/{upstream}.svg",
            }
            if stroke_px is not None and style == "outline":
                # A stroke in whole pixels, so lines centred on the even canvas stay sharp.
                units = f"{stroke_px * 24 / size:.6g}"
                svg, count = _STROKE_WIDTH.subn(f'stroke-width="{units}"', svg, count=1)
                if count != 1:
                    message = f"glyph {name!r}: no stroke-width to set"
                    raise IconError(message)
                record["stroke_px"] = stroke_px
            if exception is not None:
                record["exception"] = exception
            jobs.append((svg, f"usr/share/fplinux/glyphs/{size}/{name}.png", size, record))
    return jobs


def _profile_radii(profile: dict[str, Any]) -> dict[int, int]:
    table = profile.get("tile_radius")
    if not isinstance(table, dict) or not table:
        message = "fplinux-ui-profile.json has no tile_radius"
        raise IconError(message)
    radii: dict[int, int] = {}
    for key, radius in table.items():
        if not key.isdecimal() or int(key) <= 0 or key != str(int(key)):
            message = f"tile_radius size {key!r} must be a positive integer"
            raise IconError(message)
        size = int(key)
        if isinstance(radius, bool) or not isinstance(radius, int) or not 0 < radius <= size // 2:
            message = f"tile_radius {size} must be an integer in 1..{size // 2}"
            raise IconError(message)
        radii[size] = radius
    return radii


def _colour(value: object, label: str) -> tuple[int, int, int]:
    if not isinstance(value, str):
        message = f"{label} is not a colour"
        raise IconError(message)
    rgb = svgraster.hex_rgb(value)
    if svgraster.rgb565(rgb) != rgb:
        message = f"{label} {value} is not exact in RGB565"
        raise IconError(message)
    return rgb


def check_tile_contrast(manifest: dict[str, Any], profile: dict[str, Any]) -> list[dict[str, Any]]:
    """Check white on every tile and every tile on both theme pages."""
    targets = manifest.get("contrast", {})
    on_tile, on_page = targets.get("glyph_on_tile"), targets.get("tile_on_page")
    if not isinstance(on_tile, float | int) or not isinstance(on_page, float | int):
        message = "icons.json contrast needs glyph_on_tile and tile_on_page"
        raise IconError(message)
    themes = profile.get("themes", {})
    pages = {
        theme: svgraster.rgb565(_colour(themes.get(theme, {}).get("background"), theme))
        for theme in ("light", "dark")
    }
    tiles = profile.get("tiles", {})
    rows: list[dict[str, Any]] = []
    failures: list[str] = []
    for app, entry in sorted(manifest.get("apps", {}).items()):
        key = entry.get("tile")
        colour = svgraster.rgb565(_colour(tiles.get(key), f"tile {key}"))
        row: dict[str, Any] = {
            "app": app,
            "tile": key,
            "colour": "#{:02X}{:02X}{:02X}".format(*colour),
            "white_on_tile": round(svgraster.contrast(WHITE, colour), 2),
        }
        if row["white_on_tile"] < on_tile:
            failures.append(f"{app}: white on tile {row['white_on_tile']} < {on_tile}")
        for theme, page in pages.items():
            ratio = round(svgraster.contrast(colour, page), 2)
            row[f"tile_on_{theme}_page"] = ratio
            if ratio < on_page:
                failures.append(f"{app}: tile on {theme} page {ratio} < {on_page}")
        rows.append(row)
    if failures:
        raise IconError("contrast below target:\n" + "\n".join(failures))
    return rows


def check_selection(profile: dict[str, Any], radii: dict[int, int]) -> list[dict[str, Any]]:
    """Derive the concentric selection shapes and check their rules.

    Grid: the visible tile s - 2i (radius r - i), the gap shape s + 2(g - i)
    (radius r + g - i) and the ring's outer shape s + 2(g + w - i) (radius
    r + g + w - i), with g >= 1, 0 <= i <= g and i < r; every size even.
    """
    rows: list[dict[str, Any]] = []
    for name, values in sorted(profile["profiles"].items()):
        selection = values["selection"]
        tile, row_icon = values["app_icon_size"], values["row_icon_size"]
        gap, width = selection["grid_ring_gap"], selection["grid_ring_width"]
        inset = selection["grid_tile_inset"]
        radius = radii[tile]
        if gap < 1 or not 0 <= inset <= gap or inset >= radius:
            message = f"profile {name}: invalid grid ring gap {gap} and tile inset {inset}"
            raise IconError(message)
        list_gap, list_width = (
            selection["list_tile_ring_gap"],
            selection["list_tile_ring_width"],
        )
        result = {
            "profile": name,
            "grid_tile": [tile, radius],
            "grid_tile_inset": inset,
            "grid_tile_visible": [tile - 2 * inset, radius - inset],
            "grid_ring_inner": [tile + 2 * (gap - inset), radius + gap - inset],
            "grid_ring_outer": [
                tile + 2 * (gap + width - inset),
                radius + gap + width - inset,
            ],
            "list_tile": [row_icon, radii[row_icon]],
            "list_ring_outer": [
                row_icon + 2 * (list_gap + list_width),
                radii[row_icon] + list_gap + list_width,
            ],
        }
        boxes = (
            "grid_tile",
            "grid_tile_visible",
            "grid_ring_inner",
            "grid_ring_outer",
            "list_tile",
            "list_ring_outer",
        )
        if any(result[box][0] % 2 for box in boxes):
            message = f"profile {name}: a selection shape has an odd size, it cannot be centred"
            raise IconError(message)
        rows.append(result)
    return rows


def check_tile_distance(manifest: dict[str, Any], profile: dict[str, Any]) -> list[dict[str, Any]]:
    """Require every pair of profile tile colours to differ by the CIEDE2000 target."""
    target = manifest.get("contrast", {}).get("tile_distance")
    if not isinstance(target, float | int):
        message = "icons.json contrast needs tile_distance"
        raise IconError(message)
    tiles = {
        key: svgraster.rgb565(_colour(value, f"tile {key}"))
        for key, value in sorted(profile.get("tiles", {}).items())
    }
    rows: list[dict[str, Any]] = []
    failures: list[str] = []
    for first, second in itertools.combinations(tiles, 2):
        distance = round(
            svgraster.ciede2000(svgraster.cielab(tiles[first]), svgraster.cielab(tiles[second])),
            2,
        )
        rows.append({"tiles": [first, second], "ciede2000": distance})
        if distance < target:
            failures.append(f"tiles {first}/{second}: CIEDE2000 {distance} < {target}")
    if failures:
        raise IconError("tile colours too close:\n" + "\n".join(failures))
    return sorted(rows, key=lambda row: (row["ciede2000"], row["tiles"]))


def plan_tiles(
    manifest: dict[str, Any],
    profile: dict[str, Any],
    sources: Sources,
    template: string.Template,
    radii: dict[int, int],
) -> list[tuple[str, str, int, dict[str, Any]]]:
    """Return (svg, output path, size, record) for every application tile."""
    geometry = manifest.get("tile_glyph", {})
    jobs: list[tuple[str, str, int, dict[str, Any]]] = []
    for app, entry in sorted(manifest.get("apps", {}).items()):
        colour = profile["tiles"][entry["tile"]]
        body = _svg_body(sources.read("tabler", entry["glyph"], "outline"))
        for size, radius in sorted(radii.items()):
            box = geometry.get(str(size), {})
            glyph, stroke = box.get("glyph"), box.get("stroke")
            if not isinstance(glyph, int) or not isinstance(stroke, float | int):
                message = f"icons.json tile_glyph {size} needs an integer glyph and a stroke"
                raise IconError(message)
            if (size - glyph) % 2:
                message = f"tile {size}: glyph {glyph} is not centred on whole pixels"
                raise IconError(message)
            scale = glyph / 24
            # The rectangle is not painted: this render is the glyph layer.
            svg = template.substitute(
                size=size,
                radius=radius,
                colour="none",
                offset=(size - glyph) // 2,
                scale=f"{scale:.6g}",
                stroke=f"{stroke / scale:.6g}",
                glyph=body,
            )
            record = {
                "app": app,
                "size": size,
                "radius": radius,
                "colour": colour,
                "glyph": f"tabler:outline/{entry['glyph']}.svg",
                "glyph_px": glyph,
                "stroke_px": stroke,
                "install": bool(entry.get("install")),
            }
            jobs.append((svg, f"{size}x{size}/{app}.png", size, record))
    return jobs


def _mirrored(alpha: bytes, size: int, transform: str) -> bytes:
    out = bytearray(len(alpha))
    for y in range(size):
        for x in range(size):
            if transform == "x":
                source = y * size + (size - 1 - x)
            elif transform == "y":
                source = (size - 1 - y) * size + x
            else:
                source = x * size + y
            out[y * size + x] = alpha[source]
    return bytes(out)


def check_tile_shape(size: int, radius: int, alpha: bytes) -> dict[str, Any]:
    """Require a full-bleed rounded square, symmetric about its centre."""
    for transform in ("x", "y", "diagonal"):
        if _mirrored(alpha, size, transform) != alpha:
            message = f"tile shape {size} is not symmetric ({transform})"
            raise IconError(message)
    middle = size // 2
    edges = (alpha[middle], alpha[middle * size], alpha[middle * size + size - 1])
    if min(edges) != 255 or alpha[0] == 255:
        message = f"tile shape {size} is not full-bleed with rounded corners"
        raise IconError(message)
    area = round(sum(alpha) / 255, 2)
    exact = round(size * size - (4 - math.pi) * radius * radius, 2)
    if abs(area - exact) > 0.25:
        message = f"tile shape {size} covers {area} px, the rounded square {exact} px"
        raise IconError(message)
    return {"size": size, "radius": radius, "area_px": area, "exact_area_px": exact}


def compose_tile(colour: str, shape: bytes, glyph: Raster) -> Raster:
    """Paint the tile colour under a white glyph layer and cut the corners."""
    red, green, blue = svgraster.hex_rgb(colour)
    pixels = bytearray(len(glyph.pixels))
    for index, coverage in enumerate(shape):
        ink = glyph.pixels[index * svgraster.RGBA_BYTES + 3]
        if ink and coverage != 255:
            message = f"tile {glyph.width}: the glyph reaches the rounded corner"
            raise IconError(message)
        offset = index * svgraster.RGBA_BYTES
        for channel, value in enumerate((red, green, blue)):
            pixels[offset + channel] = (255 * ink + value * (255 - ink) + 127) // 255
        pixels[offset + 3] = coverage
    return Raster(glyph.width, glyph.height, bytes(pixels))


def _mask(raster: Raster) -> Raster:
    pixels = bytearray(len(raster.pixels))
    pixels[3 :: svgraster.RGBA_BYTES] = raster.alpha()
    return Raster(raster.width, raster.height, bytes(pixels))


def build(arguments: argparse.Namespace) -> dict[str, Any]:
    """Render, check and write every output; return the report."""
    manifest = _load_json(arguments.manifest)
    profile = _load_json(arguments.profile)
    sources = Sources(arguments.tabler, arguments.project)
    template = string.Template(arguments.tile_template.read_text(encoding="utf-8"))
    radii = _profile_radii(profile)
    contrast_rows = check_tile_contrast(manifest, profile)
    distance_rows = check_tile_distance(manifest, profile)
    selection_rows = check_selection(profile, radii)

    glyph_jobs = plan_glyphs(manifest, sources)
    tile_jobs = plan_tiles(manifest, profile, sources, template, radii)
    rasters = svgraster.render(
        [(svg, size) for svg, _path, size, _record in glyph_jobs + tile_jobs]
    )
    glyph_rasters, tile_rasters = rasters[: len(glyph_jobs)], rasters[len(glyph_jobs) :]
    for (_svg, path, size, _record), raster in zip(glyph_jobs + tile_jobs, rasters, strict=True):
        if raster.height != size:
            message = f"{path}: rendered {raster.width}x{raster.height}, wanted {size}x{size}"
            raise IconError(message)
        if not any(raster.alpha()):
            message = f"{path}: rendered empty"
            raise IconError(message)

    shapes = {size: rounded_square_alpha(size, radius) for size, radius in radii.items()}
    shape_rows = [check_tile_shape(size, radii[size], shapes[size]) for size in sorted(shapes)]

    outputs = [
        Output(path, _mask(raster), record)
        for (_svg, path, _size, record), raster in zip(glyph_jobs, glyph_rasters, strict=True)
    ]
    for (_svg, _path, size, record), layer in zip(tile_jobs, tile_rasters, strict=True):
        tile = compose_tile(record["colour"], shapes[size], layer)
        if record["install"]:
            outputs.append(
                Output(
                    f"usr/share/icons/hicolor/{size}x{size}/apps/{record['app']}.png", tile, record
                )
            )

    written = _write(arguments.out, outputs)
    report: dict[str, Any] = {
        "tabler_version": manifest.get("tabler_version"),
        "resvg": svgraster.RESVG_VERSION,
        "sources": dict(sorted(sources.used.items())),
        "outputs": written,
        "tile_contrast": contrast_rows,
        "tile_distance": distance_rows,
        "tile_shapes": shape_rows,
        "selection": selection_rows,
    }
    return report


def _write(root: Path, outputs: list[Output]) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for output in sorted(outputs, key=lambda item: item.path):
        data = svgraster.encode_png(output.raster)
        target = root / output.path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        records.append(
            {"path": output.path, "bytes": len(data), "sha256": _sha256(data), **output.record}
        )
    return records


def main() -> None:
    """Parse the command line and run the generator."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tabler", type=Path, required=True, help="Tabler icons/ directory")
    parser.add_argument("--out", type=Path, required=True, help="root for installed files")
    parser.add_argument("--manifest", type=Path, default=HERE / "icons.json")
    parser.add_argument("--profile", type=Path, default=HERE / "fplinux-ui-profile.json")
    parser.add_argument("--tile-template", type=Path, default=HERE / "tile.svg")
    parser.add_argument("--project", type=Path, default=HERE, help="directory of project SVGs")
    parser.add_argument("--report", type=Path, help="write the check report as JSON")
    arguments = parser.parse_args()
    try:
        report = build(arguments)
    except (IconError, svgraster.RasterError) as error:
        print(f"generate-icons: {error}", file=sys.stderr)
        sys.exit(1)
    text = json.dumps(report, indent=1, ensure_ascii=False) + "\n"
    if arguments.report is not None:
        arguments.report.write_text(text, encoding="utf-8")
    glyphs = sum(1 for item in report["outputs"] if "glyph" in item and "app" not in item)
    tiles = len(report["outputs"]) - glyphs
    print(f"generate-icons: {glyphs} glyphs, {tiles} installed tiles; all design checks passed")
    closest = report["tile_distance"][0]
    print(
        "  closest tile colours: {} / {}: CIEDE2000 {}".format(
            *closest["tiles"], closest["ciede2000"]
        )
    )
    for row in report["tile_contrast"]:
        print(
            "  {app}: {colour} white {white_on_tile} light page {tile_on_light_page} "
            "dark page {tile_on_dark_page}".format(**row)
        )


if __name__ == "__main__":
    main()
