# SPDX-License-Identifier: GPL-2.0-only
"""phoneME Canvas with native LVGL, controlled runtime surface and MIDP queue."""

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
def phoneme_screen(
    tmp_path_factory: pytest.TempPathFactory, lvgl_library: tuple[Path, Path]
) -> Iterator[Path]:
    headers, archive = lvgl_library
    port = ROOT / "alpine/aports/fplinux-phoneme"
    harness = ROOT / "tests/host_tool/phoneme"
    with c_build_directory(tmp_path_factory, "phoneme-screen") as directory:
        executable = directory / "phoneme-screen"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D_POSIX_C_SOURCE=200809L",
                "-isystem",
                str(headers),
                "-DLV_KCONFIG_IGNORE",
                "-DLV_LVGL_H_INCLUDE_SYSTEM",
                f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
                f"-I{harness / 'screen-compat'}",
                f"-I{ROOT / 'include'}",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{port / 'port/app'}",
                f"-I{port / 'port/canvas'}",
                f"-I{port / 'vm'}",
                str(harness / "screen-boundary.c"),
                str(port / "port/canvas/fplinux-pm-screen.c"),
                str(port / "port/canvas/fplinux-pm-canvas-core.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-frame.c"),
                str(ROOT / "lib/fplinux/fplinux-rgb565.c"),
                str(archive),
                "-lm",
            ],
            name="link phoneME Canvas, software frames and RGB565 with native LVGL",
        )
        yield executable


@pytest.mark.parametrize("scenario", ["native", "preset"])
def test_canvas_surface_resize_and_completed_paint(phoneme_screen: Path, scenario: str) -> None:
    result = run_process(
        [str(phoneme_screen), scenario],
        name=f"phoneME Canvas {scenario} surface",
        timeout=10,
        check=True,
    )
    assert result.stdout == f"{scenario}: ok\n"
    assert result.stderr == ""
