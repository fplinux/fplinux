# SPDX-License-Identifier: GPL-2.0-only
"""Linked boot-screen drawing into a bounded host memory canvas."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

SCREEN = ROOT / "bootstrap/fplinux-boot-screen"


@dataclass(frozen=True)
class BootGeometry:
    """Describe the framebuffer and expected mark placement for one canvas."""

    width: int
    height: int
    font: int
    mark_left: int
    mark_top: int
    mark_size: int


@pytest.fixture(scope="module")
def boot_canvas(tmp_path_factory: pytest.TempPathFactory) -> Iterator[dict[int, Path]]:
    """Link the real renderer; replace only the canvas and external font data."""
    with c_build_directory(tmp_path_factory, "boot-logo-canvas") as directory:
        executables = {}
        for font in (6, 7):
            executable = directory / f"boot-canvas-{font}"
            compile_c(
                executable,
                [
                    "-std=c99",
                    "-pedantic",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-DFPLINUX_BOOT_FONT={font}",
                    "-I",
                    str(SCREEN),
                    str(ROOT / "tests/host_tool/gui/boot_logo_canvas.c"),
                    str(SCREEN / "boot-screen.c"),
                ],
                name="compile boot mark memory canvas",
            )
            executables[font] = executable
        yield executables


@pytest.mark.parametrize(
    "geometry",
    [
        BootGeometry(128, 128, 6, 48, 10, 32),
        BootGeometry(128, 160, 6, 40, 13, 48),
        BootGeometry(176, 220, 7, 56, 18, 64),
        BootGeometry(240, 320, 7, 80, 26, 80),
        BootGeometry(320, 240, 7, 120, 20, 80),
    ],
)
@pytest.mark.parametrize("state", ["progress", "error"])
def test_mark_and_status_remain_visible_in_the_canvas(
    boot_canvas: dict[int, Path],
    tmp_path: Path,
    geometry: BootGeometry,
    state: str,
) -> None:
    """The mark, progress and diagnostic text occupy separate contained regions."""
    width, height, font = geometry.width, geometry.height, geometry.font
    mark_left, mark_top, mark_size = geometry.mark_left, geometry.mark_top, geometry.mark_size
    executable = boot_canvas[font]
    output = tmp_path / "canvas.rgb565"
    run_process(
        [str(executable), str(width), str(height), state, str(output)],
        name="render boot mark and diagnostic state",
        timeout=10,
        check=True,
    )
    pixels = struct.unpack(f"={width * height}H", output.read_bytes())
    background = 0x2166
    assert pixels[0] == pixels[-1] == background
    # A point inside the pearl breast and a clear corner of the original mark.
    breast_x = mark_left + mark_size * 52 // 100
    breast_y = mark_top + mark_size * 60 // 100
    assert pixels[breast_y * width + breast_x] == 0xE71B
    assert pixels[mark_top * width + mark_left] == background
    mark_rows = [
        y
        for y in range(mark_top, mark_top + mark_size)
        if any(
            pixels[y * width + x] != background for x in range(mark_left, mark_left + mark_size)
        )
    ]
    assert mark_rows
    # The bar spans three quarters of the width; synthetic text never does.
    bar_colour = 0xE30C if state == "error" else 0x18C3
    bar_rows = [y for y in range(height) if pixels[y * width + width * 7 // 8 - 1] == bar_colour]
    assert bar_rows
    assert min(bar_rows) > max(mark_rows)
    bar_y = bar_rows[len(bar_rows) // 2]
    if state == "progress":
        assert pixels[bar_y * width + width // 4] == 0xF79E
        assert pixels[bar_y * width + width * 3 // 4] == 0x18C3
    else:
        assert pixels[bar_y * width + width // 4] == 0xE30C
        assert pixels[bar_y * width + width * 3 // 4] == 0xE30C
    text_colour = 0xE30C if state == "error" else 0xF79E
    text_rows = [
        y
        for y in range(max(bar_rows) + 1, height)
        if text_colour in pixels[y * width : (y + 1) * width]
    ]
    assert len(text_rows) == (8 if font == 6 else 14)
    if state == "error":
        detail_rows = [
            y
            for y in range(max(text_rows) + 1, height)
            if 0xF79E in pixels[y * width : (y + 1) * width]
        ]
        assert len(detail_rows) == (16 if font == 6 else 28)
