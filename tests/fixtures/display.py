# SPDX-License-Identifier: GPL-2.0-only
"""Owned panel-profile setup for renderer and compiled-consumer tests."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING, Any

import pytest
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


@pytest.fixture
def mipi_dbi_source() -> bytes:
    """Supply a short MIPI DBI command sequence with an explicit delay."""
    return (ROOT / "tests/fixtures/display/mipi-dbi-panel.toml").read_bytes()


@pytest.fixture
def mipi_dbi_display(mipi_dbi_source: bytes) -> dict[str, Any]:
    return load_display(
        "demo",
        {"panel": "display/panel.toml", "boot_font": "6x8"},
        captured={"targets/demo/display/panel.toml": mipi_dbi_source},
    )
