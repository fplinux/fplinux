# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for pure MicroPythonOS layout arithmetic.

The tests execute the shipped layout helper at supported display sizes. They do not load
LVGL, apply the application patch, render pixels, or exercise a phone framebuffer.
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[1]
APORT = ROOT / "alpine/aports/fplinux-micropythonos"
LAYOUT_SOURCE = APORT / "fplinux-small-screen-layout.py"


def load_layout() -> ModuleType:
    """Load the pure layout module without making the aport a Python package."""
    specification = importlib.util.spec_from_file_location(
        "fplinux_small_screen_layout_test", LAYOUT_SOURCE
    )
    if specification is None or specification.loader is None:
        message = "cannot load small-screen layout module"
        raise RuntimeError(message)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def rectangle(position: tuple[int, int], size: tuple[int, int]) -> tuple[int, int, int, int]:
    """Return a rectangle as left, top, right, bottom coordinates."""
    left, top = position
    width, height = size
    return left, top, left + width, top + height


def fits(rect: tuple[int, int, int, int], width: int, height: int) -> bool:
    """Return whether a rectangle lies entirely in a display."""
    left, top, right, bottom = rect
    return 0 <= left <= right <= width and 0 <= top <= bottom <= height


def disjoint(first: tuple[int, int, int, int], second: tuple[int, int, int, int]) -> bool:
    """Return whether two rectangles do not overlap."""
    first_left, first_top, first_right, first_bottom = first
    second_left, second_top, second_right, second_bottom = second
    return (
        first_right <= second_left
        or second_right <= first_left
        or first_bottom <= second_top
        or second_bottom <= first_top
    )


class MicroPythonOsLayoutMathHostTests(unittest.TestCase):
    """Check helper-produced rectangles without claiming rendered UI coverage."""

    def setUp(self) -> None:
        """Load the layout helper independently for every geometry check."""
        self.layout = load_layout()

    def test_connect4_keeps_board_status_and_controls_separate(self) -> None:
        """Connect Four must fit rather than overlap its two bottom buttons."""
        for width, height in ((128, 160), (240, 320)):
            layout = self.layout.connect4_layout(width, height)
            board = rectangle(layout["board_pos"], layout["board_size"])
            status = rectangle(layout["status_pos"], (layout["status_width"], 16))
            difficulty = rectangle(layout["difficulty_pos"], layout["button_size"])
            new_game = rectangle(layout["new_game_pos"], layout["button_size"])

            for area in (board, status, difficulty, new_game):
                self.assertTrue(fits(area, width, height), (width, height, area))
            self.assertTrue(disjoint(board, status))
            self.assertTrue(disjoint(status, difficulty))
            self.assertTrue(disjoint(status, new_game))
            self.assertTrue(disjoint(difficulty, new_game))

    def test_columns_and_floodit_keep_every_control_visible(self) -> None:
        """The tall games reserve a board area before positioning controls."""
        for width, height in ((128, 160), (240, 320)):
            columns = self.layout.columns_layout(width, height)
            columns_board = rectangle(
                columns["board_pos"],
                (columns["cell_size"] * 6, columns["cell_size"] * 12),
            )
            columns_controls = [
                rectangle(columns[name], columns["control_size"])
                for name in ("left_pos", "right_pos", "rotate_pos", "down_pos")
            ]
            self.assertTrue(fits(columns_board, width, height))
            for control in columns_controls:
                self.assertTrue(fits(control, width, height))
                self.assertTrue(disjoint(columns_board, control))

            floodit = self.layout.floodit_layout(width, height)
            floodit_board = rectangle(
                floodit["board_pos"],
                (floodit["cell_size"] * 10, floodit["cell_size"] * 10),
            )
            self.assertTrue(fits(floodit_board, width, height))
            for index in range(6):
                control = rectangle(floodit["button_pos"](index), floodit["button_size"])
                self.assertTrue(fits(control, width, height))
                self.assertTrue(disjoint(floodit_board, control))

    def test_keypad_test_and_dashboards_reserve_non_overlapping_regions(self) -> None:
        """Text entry and game scoreboards must not be clipped at 128x160."""
        for width, height in ((128, 160), (240, 320)):
            keypad = self.layout.keypad_test_layout(width, height)
            text = rectangle(
                (keypad["padding"], keypad["text_top"]),
                (width - (keypad["padding"] * 2), keypad["text_height"]),
            )
            state = rectangle(
                keypad["state_pos"],
                (width - (keypad["padding"] * 2), keypad["state_height"]),
            )
            controls = rectangle(
                keypad["controls_pos"],
                (width - (keypad["padding"] * 2), keypad["controls_height"]),
            )
            for area in (text, state, controls):
                self.assertTrue(fits(area, width, height), (width, height, area))
            self.assertTrue(disjoint(text, state))
            self.assertTrue(disjoint(state, controls))

            dashboard = self.layout.game_dashboard_layout(width, height)
            board = rectangle(dashboard["board_pos"], dashboard["board_size"])
            reset_width, reset_height = dashboard["reset_size"]
            reset = rectangle(
                (
                    width - dashboard["padding"] - reset_width,
                    height - dashboard["padding"] - reset_height,
                ),
                dashboard["reset_size"],
            )
            self.assertTrue(fits(board, width, height))
            self.assertTrue(fits(reset, width, height))
            self.assertTrue(disjoint(board, reset))

    def test_toolbar_and_image_picker_geometry_fit(self) -> None:
        """Return fitting toolbar and picker geometry at supported sizes."""
        for width, height in ((128, 160), (240, 320)):
            margin = 4
            scale = self.layout.about_logo_scale(width, margin)
            self.assertLessEqual((296 * scale) // 256, width - (margin * 2))
            self.assertGreaterEqual(self.layout.icon_control_size(width), 20)

            imageview = self.layout.imageview_layout(width, height)
            self.assertGreaterEqual(imageview["label_width"], 1)
            self.assertLessEqual(
                imageview["label_width"] + imageview["button_size"][0],
                width,
            )


if __name__ == "__main__":
    unittest.main()
