#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate the UI profile and palette table of libfplinux-ui and the session.

lib/fplinux/fplinux-ui-profile.json defines the runtime profiles. After a change, run

    ./fplinux format lib/fplinux/fplinux-ui-profile.json
    python3 scripts/fplinux_ui_profile.py

to rewrite lib/fplinux/fplinux-ui-profile.c; ``--check`` only reports whether
that file is current. The structures are declared in
include/fplinux/fplinux-ui-profile.h. Every profile key is mapped or checked,
so that a new token cannot be dropped silently.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "lib/fplinux/fplinux-ui-profile.json"
OUTPUT = ROOT / "lib/fplinux/fplinux-ui-profile.c"

# The order of enum fplinux_ui_profile.
PROFILES = ("128x128", "128x160", "176x220", "240x320", "320x240")

# Each font slot field and the key of a profile's fonts object it takes.
FONT_SLOTS = (
    ("font_body", "body"),
    ("font_title", "title"),
    ("font_clock", "clock"),
    ("font_legend", "softkey"),
)
_NOTO = re.compile(r"NotoSans( Condensed| SemiCondensed)?( Bold)? ([0-9]+) 4bpp( digits)?")
# Keys of a profile's fonts object; metrics and build notes are for checks.
FONT_KEYS = frozenset({"body", "title", "clock", "softkey", "metrics", "build"})

# Integer fields named as their JSON keys, in the order of the structure.
INT_FIELDS = (
    "width",
    "height",
    "status_height",
    "softkey_height",
    "header_height",
    "margin",
    "gutter",
    "spacing",
    "gap_small",
    "content_padding",
    "list_padding",
    "scrollbar_width",
    "scrollbar_offset",
    "focus_width",
    "row_height",
    "row_height_detail",
    "row_padding",
    "row_gap",
    "row_icon_size",
    "status_icon_size",
    "app_icon_size",
    "grid_columns",
    "grid_scrollbar_offset",
    "frame_padding",
    "field_padding",
    "field_lines",
    "text_box_padding",
    "picker_columns",
    "picker_border",
    "picker_padding",
    "alert_margin",
    "alert_padding",
    "alert_gap",
    "alert_border",
    "spinner_size",
    "spinner_width",
    "bar_height",
    "menu_edge",
    "legend_margin",
    "legend_gap",
    "home_gap",
)
# Keys that are converted rather than copied; see profile_fields().
CONVERTED_KEYS = frozenset(
    {
        "fonts",
        "divider",
        "catalogue_default",
        "field_status_inline",
        "picker_cell",
        "battery",
        "logo",
        "integrated_surface",
        "picker_font",
        "selection",
        "home_hint",
    }
)
# The order of struct fplinux_ui_profile_spec after the font slots.
FIELD_ORDER = (
    "width",
    "height",
    "font_body",
    "font_title",
    "font_clock",
    "font_legend",
    "status_height",
    "softkey_height",
    "header_height",
    "margin",
    "gutter",
    "spacing",
    "gap_small",
    "content_padding",
    "list_padding",
    "scrollbar_width",
    "scrollbar_offset",
    "focus_width",
    "divider_width",
    "row_height",
    "row_height_detail",
    "row_padding",
    "row_gap",
    "row_icon_size",
    "status_icon_size",
    "app_icon_size",
    "grid_columns",
    "grid_scrollbar_offset",
    "catalogue_list",
    "row_tile_radius",
    "app_tile_radius",
    "grid_ring_width",
    "grid_ring_gap",
    "grid_tile_inset",
    "grid_ring_colour",
    "list_tile_ring_width",
    "list_tile_ring_gap",
    "list_tile_ring_colour",
    "frame_padding",
    "field_padding",
    "field_lines",
    "text_box_padding",
    "field_status_inline",
    "picker_columns",
    "picker_cell_width",
    "picker_cell_height",
    "picker_border",
    "picker_padding",
    "alert_margin",
    "alert_padding",
    "alert_gap",
    "alert_border",
    "spinner_size",
    "spinner_width",
    "bar_height",
    "menu_edge",
    "legend_margin",
    "legend_gap",
    "home_gap",
    "home_hint_replaces_date",
    "battery_width",
    "battery_height",
    "battery_cap_width",
    "battery_cap_height",
    "battery_bar_width",
    "battery_bar_height",
    "bolt_height",
    "splash_mark",
    "boot_mark",
    "about_logo_width",
    "about_lockup",
)

# The order of enum fplinux_ui_colour.
COLOURS = (
    "background",
    "surface",
    "foreground",
    "muted",
    "accent",
    "accent_text",
    "border",
    "popup",
    "bar",
    "bar_text",
    "disabled",
    "focus",
    "warning",
    "danger",
    "tile_ring",
    "brand",
    "scrim",
)
# enum fplinux_ui_palette, then the palette of each theme kind (light, dark).
PALETTES = (
    ("FPLINUX_UI_PALETTE_LIGHT", "light", 0),
    ("FPLINUX_UI_PALETTE_DARK", "dark", 1),
)
THEME_PALETTES = ("FPLINUX_UI_PALETTE_LIGHT", "FPLINUX_UI_PALETTE_DARK")
_HEX = re.compile(r"#[0-9A-Fa-f]{6}")
# The lock screen's unlock hint: where it goes, and whether it replaces the date.
HOME_HINTS = {"below the date": False, "replaces the date while shown": True}

FUNCTIONS = """\
_Static_assert(ARRAY_SIZE(profiles) == FPLINUX_UI_PROFILE_COUNT,
\t       "one entry per profile");
_Static_assert(ARRAY_SIZE(palettes) == FPLINUX_UI_PALETTE_COUNT,
\t       "one entry per palette");

/* The largest fitting pixel area wins; ABI order breaks equal-area ties. */
int fplinux_ui_profile_for_screen(int width, int height)
{
\tint profile = 0;
\tint largest_area = 0;

\tfor (int i = 0; i < FPLINUX_UI_PROFILE_COUNT; ++i) {
\t\tint area = profiles[i].width * profiles[i].height;

\t\tif (profiles[i].width <= width &&
\t\t    profiles[i].height <= height && area > largest_area) {
\t\t\tprofile = i;
\t\t\tlargest_area = area;
\t\t}
\t}
\treturn profile;
}

const struct fplinux_ui_profile_spec *fplinux_ui_profile_spec(int profile)
{
\tif (profile < 0 || profile >= FPLINUX_UI_PROFILE_COUNT)
\t\treturn NULL;
\treturn &profiles[profile];
}

const struct fplinux_ui_theme_spec *fplinux_ui_palette_spec(int palette)
{
\tif (palette < 0 || palette >= FPLINUX_UI_PALETTE_COUNT)
\t\treturn NULL;
\treturn &palettes[palette];
}

const struct fplinux_ui_theme_spec *fplinux_ui_theme_spec(int kind)
{
\tif (kind < 0 || kind >= (int)ARRAY_SIZE(theme_palettes))
\t\treturn NULL;
\treturn &palettes[theme_palettes[kind]];
}
"""


class ProfileError(ValueError):
    """The token file does not describe a valid table."""


def is_rgb565_exact(value: int) -> bool:
    """Return whether the panel shows 0xRRGGBB unchanged after RGB565."""
    red, green, blue = value >> 16, (value >> 8) & 0xFF, value & 0xFF
    return (
        red == ((red >> 3) << 3 | red >> 5)
        and green == ((green >> 2) << 2 | green >> 6)
        and blue == ((blue >> 3) << 3 | blue >> 5)
    )


def _integer(profile: str, key: str, value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ProfileError(f"{profile}: {key} must be a non-negative integer")
    return value


def _pair(profile: str, key: str, value: object) -> tuple[int, int]:
    if not isinstance(value, list) or len(value) != 2:
        raise ProfileError(f"{profile}: {key} must be [width, height]")
    return _integer(profile, key, value[0]), _integer(profile, key, value[1])


def _mapping(profile: str, key: str, value: object) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ProfileError(f"{profile}: {key} must be an object")
    return value


def font_id(profile: str, description: object) -> str:
    """Return the runtime font id of one font slot description."""
    if not isinstance(description, str):
        raise ProfileError(f"{profile}: a font slot must be a string")
    noto = _NOTO.fullmatch(description)
    if noto is None:
        raise ProfileError(f"{profile}: unknown font {description!r}")
    width = noto.group(1).strip().lower() if noto.group(1) else ""
    stretch = f"-{width}" if width else ""
    bold = "-bold" if noto.group(2) else ""
    digits = "-digits" if noto.group(4) else ""
    return f"noto-sans{stretch}{bold}-{noto.group(3)}{digits}"


def font_slots(profile: str, value: object) -> dict[str, str]:
    """Return the font description of each slot field of a profile.

    The fields are font_body, font_title, font_clock and font_legend.
    """
    fonts = _mapping(profile, "fonts", value)
    unknown = set(fonts) - FONT_KEYS
    if unknown:
        raise ProfileError(f"{profile}: font keys without a slot: {', '.join(sorted(unknown))}")
    # The soft-key legends use the body font unless a profile names another.
    descriptions = {field: fonts.get(slot, fonts.get("body")) for field, slot in FONT_SLOTS}
    for description in descriptions.values():
        if not isinstance(description, str):
            raise ProfileError(f"{profile}: a font slot must be a string")
    return {field: str(description) for field, description in descriptions.items()}


def _fonts(profile: str, value: object) -> dict[str, str]:
    return {
        field: font_id(profile, description)
        for field, description in font_slots(profile, value).items()
    }


def _about(profile: str, value: object) -> tuple[int, bool]:
    if not isinstance(value, str):
        raise ProfileError(f"{profile}: logo.about must be a string")
    mark = re.fullmatch(r"mark ([0-9]+)", value)
    if mark:
        return int(mark.group(1)), False
    lockup = re.fullmatch(r"lockup ([0-9]+) wide", value)
    if lockup:
        return int(lockup.group(1)), True
    raise ProfileError(f"{profile}: unknown logo.about {value!r}")


def _tile_radius(profile: str, radii: dict[str, Any], size: object) -> int:
    return _integer(profile, f"tile_radius.{size}", radii.get(str(size)))


def _selection(profile: str, value: object) -> dict[str, int | str | bool]:
    selection = _mapping(profile, "selection", value)
    fields: dict[str, int | str | bool] = {}
    for ring in ("grid_ring", "list_tile_ring"):
        fields[f"{ring}_width"] = _integer(
            profile, f"{ring}_width", selection.get(f"{ring}_width")
        )
        fields[f"{ring}_gap"] = _integer(profile, f"{ring}_gap", selection.get(f"{ring}_gap"))
        colour = selection.get(f"{ring}_colour")
        if colour not in COLOURS:
            raise ProfileError(f"{profile}: selection.{ring}_colour must name a colour token")
        fields[f"{ring}_colour"] = f"FPLINUX_UI_COLOUR_{str(colour).upper()}"
    fields["grid_tile_inset"] = _integer(
        profile, "grid_tile_inset", selection.get("grid_tile_inset")
    )
    if set(selection) != set(fields):
        raise ProfileError(f"{profile}: selection keys must be {', '.join(sorted(fields))}")
    return fields


def profile_fields(
    name: str, profile: dict[str, Any], tile_radii: dict[str, Any]
) -> dict[str, int | str | bool]:
    """Return the structure fields of one profile, keyed by field name."""
    unknown = set(profile) - set(INT_FIELDS) - CONVERTED_KEYS
    if unknown:
        raise ProfileError(f"{name}: keys without a field: {', '.join(sorted(unknown))}")
    missing = (set(INT_FIELDS) | CONVERTED_KEYS) - set(profile)
    if missing:
        raise ProfileError(f"{name}: missing keys: {', '.join(sorted(missing))}")
    fields: dict[str, int | str | bool] = {
        key: _integer(name, key, profile[key]) for key in INT_FIELDS
    }
    if name != f"{fields['width']}x{fields['height']}":
        raise ProfileError(f"{name}: width and height disagree with the name")
    fields.update(_fonts(name, profile["fonts"]))
    fields["divider_width"] = _integer(name, "divider", profile["divider"])
    catalogue = profile["catalogue_default"]
    if catalogue not in {"grid", "list"}:
        raise ProfileError(f"{name}: catalogue_default must be grid or list")
    fields["catalogue_list"] = catalogue == "list"
    inline = profile["field_status_inline"]
    if not isinstance(inline, bool):
        raise ProfileError(f"{name}: field_status_inline must be true or false")
    fields["field_status_inline"] = inline
    cell = _pair(name, "picker_cell", profile["picker_cell"])
    fields["picker_cell_width"], fields["picker_cell_height"] = cell
    battery = _mapping(name, "battery", profile["battery"])
    fields["battery_width"], fields["battery_height"] = _pair(
        name, "battery.body", battery.get("body")
    )
    fields["battery_cap_width"], fields["battery_cap_height"] = _pair(
        name, "battery.cap", battery.get("cap")
    )
    fields["battery_bar_width"], fields["battery_bar_height"] = _pair(
        name, "battery.bar", battery.get("bar")
    )
    fields["bolt_height"] = _integer(name, "battery.bolt_height", battery.get("bolt_height"))
    logo = _mapping(name, "logo", profile["logo"])
    fields["splash_mark"] = _integer(name, "logo.splash_mark", logo.get("splash_mark"))
    fields["boot_mark"] = _integer(name, "logo.boot_mark", logo.get("boot_mark"))
    fields["about_logo_width"], fields["about_lockup"] = _about(name, logo.get("about"))
    fields["row_tile_radius"] = _tile_radius(name, tile_radii, fields["row_icon_size"])
    fields["app_tile_radius"] = _tile_radius(name, tile_radii, fields["app_icon_size"])
    fields.update(_selection(name, profile["selection"]))
    if profile["home_hint"] not in HOME_HINTS:
        raise ProfileError(f"{name}: home_hint must be one of {', '.join(HOME_HINTS)}")
    fields["home_hint_replaces_date"] = HOME_HINTS[profile["home_hint"]]
    # Derived and fixed values are checked instead of stored.
    surface = _pair(name, "integrated_surface", profile["integrated_surface"])
    width, height = int(fields["width"]), int(fields["height"])
    bars = int(fields["status_height"]) + int(fields["softkey_height"])
    expected = (width, height - bars)
    if surface != expected:
        raise ProfileError(f"{name}: integrated_surface must be {list(expected)}")
    if profile["picker_font"] != "font_large":
        raise ProfileError(f"{name}: the picker has only the font_large slot")
    if set(fields) != set(FIELD_ORDER):
        raise ProfileError(f"{name}: fields differ from FIELD_ORDER")
    return fields


def palette(name: str, theme: object) -> list[int]:
    """Return a palette's colours in the order of enum fplinux_ui_colour."""
    colours = _mapping(name, "theme", theme)
    if set(colours) != set(COLOURS):
        raise ProfileError(f"{name}: tokens must be exactly {', '.join(COLOURS)}")
    values: list[int] = []
    for token in COLOURS:
        text = colours[token]
        if not isinstance(text, str) or not _HEX.fullmatch(text):
            raise ProfileError(f"{name}: {token} must be #RRGGBB")
        value = int(text[1:], 16)
        if not is_rgb565_exact(value):
            raise ProfileError(f"{name}: {token} {text} is not exact in RGB565")
        values.append(value)
    return values


def modal_opacity(value: object) -> int:
    """Return the static backdrop opacity shared by both theme palettes."""
    modal = _mapping("token file", "modal", value)
    if set(modal) != {"scrim_opacity"}:
        message = "modal must contain exactly scrim_opacity"
        raise ProfileError(message)
    opacity = _integer("modal", "scrim_opacity", modal["scrim_opacity"])
    if not 0 < opacity < 255:
        message = "modal: scrim_opacity must dim, not hide, the background (1..254)"
        raise ProfileError(message)
    return opacity


def _literal(value: int | str | bool) -> str:  # noqa: FBT001 -- one table cell.
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        # Colour tokens are enumerators; everything else is a string.
        return value if value.startswith("FPLINUX_UI_COLOUR_") else json.dumps(value)
    return str(value)


def render(data: object) -> str:
    """Return the C source of the table described by the token file."""
    document = _mapping("token file", "document", data)
    profiles = _mapping("token file", "profiles", document.get("profiles"))
    if tuple(profiles) != PROFILES:
        raise ProfileError(f"profiles must be {', '.join(PROFILES)} in this order")
    tile_radii = _mapping("token file", "tile_radius", document.get("tile_radius"))
    themes = _mapping("token file", "themes", document.get("themes"))
    scrim_opacity = modal_opacity(document.get("modal"))
    lines = [
        # The generated file's licence tag, not this script's.
        # REUSE-IgnoreStart
        "/* SPDX-License-Identifier: LGPL-2.1-or-later */",
        # REUSE-IgnoreEnd
        "/*",
        " * Generated by scripts/fplinux_ui_profile.py from",
        " * lib/fplinux/fplinux-ui-profile.json; change those and regenerate.",
        " */",
        '#include "fplinux-ui-profile.h"',
        "",
        "#include <stddef.h>",
        "",
        "#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))",
        "",
        "static const struct fplinux_ui_profile_spec profiles[] = {",
    ]
    for name in PROFILES:
        profile = _mapping(name, "profile", profiles[name])
        fields = profile_fields(name, profile, tile_radii)
        lines.append(f"\t/* {name} */")
        lines.append("\t{")
        lines.extend(f"\t\t.{field} = {_literal(fields[field])}," for field in FIELD_ORDER)
        lines.append("\t},")
    lines.extend(("};", "", "static const struct fplinux_ui_theme_spec palettes[] = {"))
    for constant, name, kind in PALETTES:
        values = palette(name, themes.get(name))
        lines.append(f"\t[{constant}] = {{")
        lines.append(f"\t\t.kind = {kind},")
        lines.append("\t\t.rgb = {")
        lines.extend(
            f"\t\t\t[FPLINUX_UI_COLOUR_{token.upper()}] = 0x{value:06x},"
            for token, value in zip(COLOURS, values, strict=True)
        )
        lines.extend(("\t\t},", f"\t\t.scrim_opacity = {scrim_opacity},", "\t},"))
    lines.extend(("};", "", "/* The palette of each theme kind: light, dark. */"))
    lines.append("static const enum fplinux_ui_palette theme_palettes[] = {")
    lines.extend(f"\t{constant}," for constant in THEME_PALETTES)
    lines.extend(("};", "", FUNCTIONS))
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="only report whether the C table is current"
    )
    arguments = parser.parse_args(argv)
    try:
        source = render(json.loads(SOURCE.read_text(encoding="utf-8")))
    except (OSError, ValueError) as error:
        print(f"fplinux_ui_profile: {error}", file=sys.stderr)
        return 1
    if arguments.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != source:
            print(f"fplinux_ui_profile: {OUTPUT.relative_to(ROOT)} is stale", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(source, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
