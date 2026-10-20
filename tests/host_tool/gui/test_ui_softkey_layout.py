# SPDX-License-Identifier: GPL-2.0-only
"""Soft-key geometry and UTF-8 fitting with a controlled text measurer."""

from pathlib import Path

import pytest

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture
def softkey_layout(tmp_path: Path) -> Path:
    """Link the production geometry without a display or LVGL drawing."""
    executable = tmp_path / "softkey-layout"
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            str(ROOT / "tests/host_tool/gui/fplinux-ui-softkey-layout.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-softkey-layout.c"),
            "-o",
            str(executable),
        ],
        name="compile soft-key layout",
        timeout=30,
        check=True,
    )
    return executable


@pytest.mark.parametrize("scenario", ["layout", "fitting"])
def test_softkey_geometry_and_utf8_output(softkey_layout: Path, scenario: str) -> None:
    """Legends retain their slots and shortened text ends at code points."""
    run_process(
        [str(softkey_layout), scenario],
        name=f"run soft-key {scenario}",
        timeout=10,
        check=True,
    )
