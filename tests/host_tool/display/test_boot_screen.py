# SPDX-License-Identifier: GPL-2.0-only
"""Linked host rendering checks for the target-selected bootstrap font."""

from __future__ import annotations

import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SCREEN = ROOT / "bootstrap/fplinux-boot-screen"


class BootScreenFontTests:
    """The same compiled glyph remains selected when the canvas width changes."""

    executables: dict[int, Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Build both selected-font variants once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            cls.executables = {}
            for font in (6, 7):
                cls.executables[font] = Path(build_directory.name) / f"boot-screen-{font}"
                run_process(
                    [
                        "cc",
                        "-std=c99",
                        "-pedantic",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        f"-DFPLINUX_BOOT_FONT={font}",
                        "-I",
                        str(SCREEN),
                        str(ROOT / "tests/host_tool/display/fplinux-boot-screen.c"),
                        str(SCREEN / "boot-screen.c"),
                        "-o",
                        str(cls.executables[font]),
                    ],
                    name="compile selected boot font harness",
                    timeout=30,
                    check=True,
                )
            yield

    @pytest.mark.parametrize(
        ("font", "glyph"),
        [
            pytest.param(
                6, ("#....#", ".#..#.", "..##..", "..##..", ".#..#.", "#....#"), id="font-6"
            ),
            pytest.param(
                7,
                (
                    "#.....#",
                    ".#...#.",
                    "..#.#..",
                    "...#...",
                    "...#...",
                    "...#...",
                    "...#...",
                    "...#...",
                    "...#...",
                    "...#...",
                    "...#...",
                    "..#.#..",
                    ".#...#.",
                    "#.....#",
                ),
                id="font-7",
            ),
        ],
    )
    @pytest.mark.parametrize("character", ["\x01", "\x7f"], ids=["x01", "x7f"])
    def test_selected_font_renders_its_glyph_on_both_canvas_sizes(
        self, font: int, glyph: tuple[str, ...], character: str
    ) -> None:
        """Synthetic glyph pixels distinguish 6x8 and 7x14 independently of layout."""
        result = run_process(
            [str(self.executables[font])],
            name="render selected boot glyph",
            timeout=10,
            check=True,
        )
        frames = result.stdout.split("canvas ")[1:]
        assert (len(frames)) == (2)
        for frame in frames:
            rectangles = [tuple(map(int, line.split())) for line in frame.splitlines()[1:]]
            assert rectangles
            assert all(width == height == 2 for _, _, width, height in rectangles)
            left = min(x for x, _, _, _ in rectangles)
            top = min(y for _, y, _, _ in rectangles)
            pixels = {((x - left) // 2, (y - top) // 2) for x, y, _, _ in rectangles}
            width = max(x for x, _ in pixels) + 1
            rendered = tuple(
                "".join("#" if (x, y) in pixels else "." for x in range(width))
                for y in range(max(y for _, y in pixels) + 1)
            )
            assert (rendered) == (glyph)
        question = run_process(
            [str(self.executables[font]), "?"],
            name="render ASCII replacement glyph",
            timeout=10,
            check=True,
        ).stdout
        replaced = run_process(
            [str(self.executables[font]), character],
            name="render character outside printable ASCII",
            timeout=10,
            check=True,
        ).stdout
        assert (replaced) == (question)
