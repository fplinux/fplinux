# SPDX-License-Identifier: GPL-2.0-only
"""Read captured RGB565 frames and measure an analog clock dial by its colours alone."""

from __future__ import annotations

import struct
from dataclasses import dataclass


def rgb565(colour: str) -> int:
    """Pack a #RRGGBB theme colour into the RGB565 framebuffer format."""
    red, green, blue = (int(colour[i : i + 2], 16) for i in (1, 3, 5))
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)


def pixel(frame: bytes, width: int, x: int, y: int) -> int:
    """Return the packed little-endian RGB565 value at x, y."""
    return int(struct.unpack_from("<H", frame, 2 * (y * width + x))[0])


@dataclass(frozen=True)
class Dial:
    """The ring's box and the vertical second hand, in frame pixels.

    Centres are doubled so that a centre between two pixels stays an integer.
    """

    left: int
    top: int
    right: int
    bottom: int
    second_column: int
    second_top: int
    second_bottom: int

    @property
    def centre_x2(self) -> int:
        """Return twice the ring's horizontal centre."""
        return self.left + self.right + 1

    @property
    def centre_y2(self) -> int:
        """Return twice the ring's vertical centre."""
        return self.top + self.bottom + 1


def measure_dial(frame: bytes, width: int, first_row: int, background: int, second: int) -> Dial:
    """Find the dial below first_row.

    Its ring is the first ink, and it ends above the first all-background
    row; the second hand is its one colour.
    """
    rows = len(frame) // (2 * width)

    def ink(y: int) -> list[int]:
        return [x for x in range(width) if pixel(frame, width, x, y) != background]

    top = next(y for y in range(first_row, rows) if ink(y))
    bottom = next(y for y in range(top, rows) if not ink(y)) - 1
    columns = [x for y in range(top, bottom + 1) for x in ink(y)]
    hand = [
        (x, y)
        for y in range(top, bottom + 1)
        for x in range(width)
        if pixel(frame, width, x, y) == second
    ]
    hand_columns = {x for x, _ in hand}
    hand_rows = sorted(y for _, y in hand)
    assert len(hand_columns) == 1, f"the second hand is not one column: {sorted(hand_columns)}"
    assert hand_rows == list(range(hand_rows[0], hand_rows[-1] + 1))
    return Dial(
        left=min(columns),
        top=top,
        right=max(columns),
        bottom=bottom,
        second_column=hand_columns.pop(),
        second_top=hand_rows[0],
        second_bottom=hand_rows[-1],
    )


def assert_hands_pivot_on_centre(frame: bytes, width: int, dial: Dial, hand: int) -> None:
    """Check that the upright hands pivot on the dial's centre.

    Upright 2 px hands straddle the centre and end on its row; the 1 px
    second hand of the same length takes the column before the centre.
    """
    assert dial.right - dial.left == dial.bottom - dial.top, "the ring is not square"
    assert 2 * dial.second_column + 2 == dial.centre_x2
    assert 2 * (dial.second_bottom + 1) == dial.centre_y2
    beside = dial.second_column + 1
    minute = [
        y
        for y in range(dial.top, dial.second_bottom + 1)
        if pixel(frame, width, beside, y) == hand
    ]
    assert minute == list(range(dial.second_top, dial.second_bottom + 1))
