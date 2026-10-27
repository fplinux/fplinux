# SPDX-License-Identifier: GPL-2.0-only
"""Owned RGB565 software frames linked to the real LVGL canvas and timers."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def ui_frame(
    tmp_path_factory: pytest.TempPathFactory, lvgl_library: tuple[Path, Path]
) -> Iterator[Path]:
    """Replace only the UI screen and visibility boundaries, retaining native LVGL."""
    headers, archive = lvgl_library
    with c_build_directory(tmp_path_factory, "ui-frame") as directory:
        executable = directory / "ui-frame"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-isystem",
                str(headers),
                "-DLV_KCONFIG_IGNORE",
                "-DLV_LVGL_H_INCLUDE_SYSTEM",
                f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
                f"-I{ROOT / 'include/fplinux'}",
                str(ROOT / "tests/host_tool/gui/fplinux-ui-frame.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-frame.c"),
                str(archive),
                "-Wl,--wrap=calloc",
                "-lm",
            ],
            name="link software frames with native LVGL canvases and timers",
        )
        yield executable


@pytest.mark.parametrize("scenario", ["direct", "staged", "resize", "timer", "lifetime"])
def test_owned_software_frame_contract(ui_frame: Path, scenario: str) -> None:
    """Drawing, publication, geometry and borrowed resources keep their contracts."""
    result = run_process(
        [str(ui_frame), scenario],
        name=f"native software frame {scenario}",
        timeout=10,
        check=True,
    )
    assert result.stdout == f"{scenario}: ok\n"
