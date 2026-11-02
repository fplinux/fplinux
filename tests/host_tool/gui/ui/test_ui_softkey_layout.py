# SPDX-License-Identifier: GPL-2.0-only
"""Soft-key geometry from natural legend widths."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


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
            str(ROOT / "tests/host_tool/gui/ui/fplinux-ui-softkey-layout.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-softkey-layout.c"),
            "-o",
            str(executable),
        ],
        name="compile soft-key layout",
        timeout=30,
        check=True,
    )
    return executable


def test_softkey_slots_keep_short_legends_and_share_the_rest(softkey_layout: Path) -> None:
    """Narrow legends keep their widths; long ones share the room left in the bar."""
    run_process(
        [str(softkey_layout), "layout"],
        name="run soft-key layout",
        timeout=10,
        check=True,
    )
