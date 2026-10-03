# SPDX-License-Identifier: GPL-2.0-only
"""Linked host rendering checks for the target-selected bootstrap font."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
SCREEN = ROOT / "bootstrap/fplinux-boot-screen"


class BootScreenFontTests(unittest.TestCase):
    """The same compiled glyph remains selected when the canvas width changes."""

    def test_selected_font_renders_its_glyph_on_both_canvas_sizes(self) -> None:
        """Synthetic glyph pixels distinguish 6x8 and 7x14 independently of layout."""
        expected = {
            6: ("#....#", ".#..#.", "..##..", "..##..", ".#..#.", "#....#"),
            7: (
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
        }
        with tempfile.TemporaryDirectory() as directory:
            for font, glyph in expected.items():
                with self.subTest(font=font):
                    executable = Path(directory) / f"boot-screen-{font}"
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
                            str(ROOT / "tests/host_tool/fplinux-boot-screen.c"),
                            str(SCREEN / "boot-screen.c"),
                            "-o",
                            str(executable),
                        ],
                        name="compile selected boot font harness",
                        timeout=30,
                        check=True,
                    )
                    result = run_process(
                        [str(executable)],
                        name="render selected boot glyph",
                        timeout=10,
                        check=True,
                    )
                    frames = result.stdout.split("canvas ")[1:]
                    self.assertEqual(len(frames), 2)
                    for frame in frames:
                        rectangles = [
                            tuple(map(int, line.split())) for line in frame.splitlines()[1:]
                        ]
                        self.assertTrue(rectangles)
                        self.assertTrue(
                            all(width == height == 2 for _, _, width, height in rectangles)
                        )
                        left = min(x for x, _, _, _ in rectangles)
                        top = min(y for _, y, _, _ in rectangles)
                        pixels = {((x - left) // 2, (y - top) // 2) for x, y, _, _ in rectangles}
                        width = max(x for x, _ in pixels) + 1
                        rendered = tuple(
                            "".join("#" if (x, y) in pixels else "." for x in range(width))
                            for y in range(max(y for _, y in pixels) + 1)
                        )
                        self.assertEqual(rendered, glyph)
                    question = run_process(
                        [str(executable), "?"],
                        name="render ASCII replacement glyph",
                        timeout=10,
                        check=True,
                    ).stdout
                    for character in ("\x01", "\x7f"):
                        with self.subTest(character=ord(character)):
                            replaced = run_process(
                                [str(executable), character],
                                name="render character outside printable ASCII",
                                timeout=10,
                                check=True,
                            ).stdout
                            self.assertEqual(replaced, question)


if __name__ == "__main__":
    unittest.main()
