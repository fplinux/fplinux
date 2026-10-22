# SPDX-License-Identifier: GPL-2.0-only
"""Host-compiled screen selection, layout and theme profiles."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any

import fplinux_ui_profile
import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

SCREENS = {
    "128x128": 0,
    "128x160": 1,
    "176x220": 2,
    "240x320": 3,
    "320x240": 4,
    "127x320": 0,
    "320x127": 0,
    "175x219": 1,
    "239x319": 2,
    "320x320": 3,
    "640x480": 3,
}


def read_profile_table(source: Path, temporary: Path) -> dict[str, Any]:
    """Link the production profile table and read it through the C consumer."""
    executable = temporary / "ui-profile"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/fplinux-ui-profile.c"),
            str(source),
        ],
        name="compile profile table consumer",
    )
    result = run_process(
        [str(executable), *SCREENS], name="read profile table", timeout=10, check=True
    )
    table = json.loads(result.stdout)
    assert isinstance(table, dict)
    return table


@pytest.fixture(scope="module")
def profile_table(tmp_path_factory: pytest.TempPathFactory) -> dict[str, Any]:
    with c_build_directory(tmp_path_factory, "ui-profile") as temporary:
        return read_profile_table(ROOT / "lib/fplinux/fplinux-ui-profile.c", temporary)


def test_screen_selection_uses_fitting_area_and_stable_tie_order(
    profile_table: dict[str, Any],
) -> None:
    assert profile_table["selection"] == SCREENS


def test_unknown_profile_and_theme_ids_return_no_entry(profile_table: dict[str, Any]) -> None:
    assert profile_table["unknown"] == [True, True, True, True]


def test_landscape_layout_preserves_horizontal_capacity(profile_table: dict[str, Any]) -> None:
    portrait, landscape = profile_table["profiles"][3:5]
    assert landscape["width"] > landscape["height"]
    assert landscape["grid_columns"] > portrait["grid_columns"]
    bands = ("status_height", "softkey_height", "header_height")
    assert sum(landscape[name] for name in bands) < sum(portrait[name] for name in bands)
    assert landscape["grid_columns"] * landscape["app_icon_size"] <= landscape["width"]


def test_layout_holds_navigation_bands_and_three_rows(profile_table: dict[str, Any]) -> None:
    for profile in profile_table["profiles"]:
        bands = profile["status_height"] + profile["softkey_height"] + profile["header_height"]
        assert bands + 2 * profile["list_padding"] + 3 * profile["row_height"] <= profile["height"]
        ring = profile["app_icon_size"] + 2 * (
            profile["grid_ring_gap"] + profile["grid_ring_width"] - profile["grid_tile_inset"]
        )
        assert profile["grid_ring_gap"] >= 1
        assert ring <= profile["width"] // profile["grid_columns"]


def test_themes_select_distinct_colours_with_translucent_scrim(
    profile_table: dict[str, Any],
) -> None:
    light, dark = profile_table["themes"]
    assert light["background"] != dark["background"]
    assert light["foreground"] != dark["foreground"]
    for theme in profile_table["themes"]:
        assert theme["foreground"] != theme["background"]
    for opacity in profile_table["scrim_opacities"]:
        assert 0 < opacity < 255


def test_token_changes_reach_the_compiled_consumer(tmp_path: Path) -> None:
    tokens = json.loads((ROOT / "lib/fplinux/fplinux-ui-profile.json").read_text())
    tokens["profiles"]["128x128"]["margin"] = 9
    tokens["profiles"]["128x128"]["fonts"]["body"] = "NotoSans 15 4bpp"
    tokens["tile_radius"]["16"] = 4
    tokens["tile_radius"]["32"] = 8
    tokens["themes"]["light"]["background"] = "#FFFFFF"
    tokens["themes"]["dark"]["foreground"] = "#FFFFFF"
    tokens["modal"]["scrim_opacity"] = 73
    source = tmp_path / "profile.c"
    source.write_text(fplinux_ui_profile.render(tokens))
    table = read_profile_table(source, tmp_path)
    assert table["profiles"][0]["margin"] == 9
    assert table["profiles"][0]["font_body"] == "noto-sans-15"
    assert table["profiles"][0]["row_tile_radius"] == 4
    assert table["profiles"][0]["app_tile_radius"] == 8
    assert table["profiles"][1]["row_tile_radius"] == 4
    assert table["profiles"][1]["app_tile_radius"] == 8
    assert table["themes"][0]["background"] == 0xFFFFFF
    assert table["themes"][1]["foreground"] == 0xFFFFFF
    assert table["scrim_opacities"] == [73, 73]
