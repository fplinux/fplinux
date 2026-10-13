# SPDX-License-Identifier: GPL-2.0-only
"""Panel profile loading and rendered consumer-data boundaries."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build.display import render_bootstrap_display, render_linux_display_files
from fplinux_cli.manifests.display import load_display

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def panel_root(tmp_path: Path) -> Path:
    """Supply owned panel data, without ambient target or build state."""
    path = tmp_path / "targets/demo/display/panel.toml"
    path.parent.mkdir(parents=True)
    shutil.copyfile(ROOT / "tests/fixtures/display/panel.toml", path)
    return tmp_path


@pytest.fixture(name="panel_display")
def panel_profile(panel_root: Path) -> dict[str, Any]:
    return load_display(
        "demo", {"panel": "display/panel.toml", "boot_font": "7x14"}, root=panel_root
    )


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


@pytest.mark.parametrize(
    ("before", "after", "diagnostic"),
    [
        ("data = [0x55]", "data = [256]", "data[0]"),
        ("delay_ms = 120", "delay_ms = 8192", "delay_ms"),
        ("width = 128", "width = 0", "width"),
    ],
)
def test_invalid_panel_values_fail_before_rendering(
    panel_root: Path, before: str, after: str, diagnostic: str
) -> None:
    path = panel_root / "targets/demo/display/panel.toml"
    path.write_text(path.read_text(encoding="utf-8").replace(before, after), encoding="utf-8")
    with pytest.raises(SystemExit) as error:
        load_display("demo", {"panel": "display/panel.toml", "boot_font": "6x8"}, root=panel_root)
    assert diagnostic in str(error.value)
