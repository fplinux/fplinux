# SPDX-License-Identifier: GPL-2.0-only
"""Panel profile loading and rendered consumer-data boundaries."""

from __future__ import annotations

import tomllib
from typing import TYPE_CHECKING, Any

import pytest
import tomlkit
from fplinux_cli.build.display import render_bootstrap_display, render_linux_display_files
from fplinux_cli.manifests.display import load_display

from tests.fixtures.display import (
    mipi_dbi_display as mipi_dbi_display,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.fixtures.display import (
    mipi_dbi_source as mipi_dbi_source,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.fixtures.display import (
    panel_profile as panel_profile,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.fixtures.display import (
    panel_root as panel_root,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from pathlib import Path


def test_panel_geometry_and_commands_use_the_supplied_checkout(
    panel_display: dict[str, Any],
) -> None:
    assert (panel_display["width"], panel_display["height"], panel_display["boot_font"]) == (
        128,
        160,
        "7x14",
    )
    assert panel_display["panel"]["linux"]["wake_finish"] == [
        {"command": 0x36, "data": [0x48], "delay_ms": 0},
        {"command": 0x11, "data": [], "delay_ms": 200},
    ]
    assert panel_display["panel"]["bootstrap"]["commands_after"] == [
        {"command": 0x11, "data": [], "delay_ms": 120},
        {"command": 0x29, "data": [], "delay_ms": 0},
    ]


def test_headless_display_generates_no_panel_consumers() -> None:
    display = load_display("demo", {"boot_font": "6x8"})
    assert (display["width"], display["height"], display["panel"]) == (0, 0, None)
    assert render_bootstrap_display(display) is None
    assert render_linux_display_files("demo", display) == {}


def test_mipi_dbi_renderer_encodes_commands_delay_and_geometry(
    mipi_dbi_display: dict[str, Any],
) -> None:
    files = render_linux_display_files("demo", mipi_dbi_display)
    assert files == {
        "firmware/demo-panel.bin": (b"MIPI DBI\0\0\0\0\0\0\0\1\x11\0\0\1\x78\x3a\1\x55\x29\0"),
        "arch/arm/boot/dts/unisoc/fplinux-demo-panel.dtsi": (
            # REUSE-IgnoreStart
            b"// SPDX-License-Identifier: GPL-2.0-only\n"
            # REUSE-IgnoreEnd
            b"/* Generated from the target panel profile. */\n"
            b"hactive = <240>;\nvactive = <320>;\n"
        ),
    }
    assert render_bootstrap_display(mipi_dbi_display) is None


def test_mipi_dbi_renderer_preserves_multibyte_data_including_zero(
    mipi_dbi_display: dict[str, Any],
) -> None:
    mipi_dbi_display["panel"]["commands"] = [
        {"command": 0xB0, "data": [0x00, 0x7F, 0xFF], "delay_ms": 0}
    ]

    files = render_linux_display_files("demo", mipi_dbi_display)

    assert files["firmware/demo-panel.bin"] == (b"MIPI DBI\0\0\0\0\0\0\0\1\xb0\x03\x00\x7f\xff")


@pytest.mark.parametrize(
    ("updates", "diagnostic"),
    [
        ({"renderer": "other"}, "renderer"),
        ({"firmware": "../panel.bin"}, "firmware"),
        ({"commands": [{"command": 0x11, "delay_ms": 256}]}, "delay_ms"),
        ({"commands": [{"command": 0, "data": [0x55]}]}, "reserved for delays"),
        ({"commands": [{"command": 0x3A, "data": [256]}]}, "data[0]"),
        ({"width": True}, "width"),
        ({"height": 0}, "height"),
    ],
)
def test_invalid_mipi_dbi_inputs_fail_at_the_manifest_boundary(
    mipi_dbi_source: bytes, updates: dict[str, Any], diagnostic: str
) -> None:
    document = tomllib.loads(mipi_dbi_source.decode("ascii"))
    document.update(updates)
    source = tomlkit.dumps(document).encode("ascii")
    with pytest.raises(SystemExit) as error:
        load_display(
            "demo",
            {"panel": "display/panel.toml", "boot_font": "6x8"},
            captured={"targets/demo/display/panel.toml": source},
        )
    assert diagnostic in str(error.value)


@pytest.mark.parametrize(
    ("updates", "diagnostic"),
    [
        ({"commands": [{"command": 0x3A, "data": [256]}]}, "data[0]"),
        ({"commands": [{"command": 0x11, "delay_ms": 8192}]}, "delay_ms"),
        ({"width": 0}, "width"),
    ],
)
def test_invalid_panel_values_fail_before_rendering(
    panel_root: Path, updates: dict[str, Any], diagnostic: str
) -> None:
    path = panel_root / "targets/demo/display/panel.toml"
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    document.update(updates)
    path.write_text(tomlkit.dumps(document), encoding="utf-8")
    with pytest.raises(SystemExit) as error:
        load_display("demo", {"panel": "display/panel.toml", "boot_font": "6x8"}, root=panel_root)
    assert diagnostic in str(error.value)
