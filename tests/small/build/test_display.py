# SPDX-License-Identifier: GPL-2.0-only
"""Panel profile loading and rendered consumer-data boundaries."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build.display import render_bootstrap_display, render_linux_display_files
from fplinux_cli.manifests.display import load_display

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def panel_root(tmp_path: Path) -> Path:
    """Supply owned panel data, without ambient target or build state."""
    path = tmp_path / "targets/demo/display/panel.toml"
    path.parent.mkdir(parents=True)
    path.write_text(
        """width = 128
height = 160
id = 0x1234
id_mask = 0xffff
memory_access = 0x48
transport = "dbi"
license = "GPL-2.0-only"
copyright = []
commands = [{ command = 0x3a, data = [0x55] }]
[bootstrap]
spi_hz = 0
prepare_pins = true
reset_ms = 16
commands_after = "cold_finish"
[linux]
commands_after = [{ command = 0xc6, data = [0x18] }]
cold_finish = [{ command = 0x11, delay_ms = 120 }, { command = 0x29 }]
wake_finish = [{ command = 0x36, data = [0x48] }, { command = 0x11, delay_ms = 200 }]
[dbi]
timing_ns = [50, 55, 110, 20, 70, 35]
[pins]
control_base = 0xb0
control = [[0, 0x10, 0x182004], [4, 0x30, 0x82046]]
data_base = 0x1f8
data = [[0, 0x10, 0x182004]]
""",
        encoding="utf-8",
    )
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
