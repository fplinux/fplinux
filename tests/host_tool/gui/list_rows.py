# SPDX-License-Identifier: GPL-2.0-only
"""Find list rows in actual RGB565 frames by their 1 px dividers.

A list row of pitch p ends in a divider line, so the dividers in a frame
give the row boundaries. The list area ends at the line of the bar below
it, which stands for the divider of a row ending there: a row whose last
line lies under the bar's line is whole, and a divider directly above that
line is a doubled line. A part row at either end of the list area is
acceptable only when both its shown and its hidden part are at least p/4,
and a scroll step between two frames, the shift of the boundaries modulo
p, is likewise none or at least p/4. Lists start flush at the area's top,
so space above the first whole row is a fault as well. The selected row's
highlight reaches the line above it, a divider or the line above the area,
and leaves no background line between itself and the bar's line; only the
last row, with no divider of its own, may end in a background line above
empty list space.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from itertools import pairwise


@dataclass(frozen=True)
class ListArea:
    """The independent geometry of one list surface in a frame."""

    width: int
    # The first line below the line that ends the area above.
    top: int
    # The bar's line below the area; the frame may end above it.
    bottom: int
    pitch: int
    # Columns left of every row's content, where only fill and dividers show.
    edge: tuple[int, int]
    divider: int
    highlight: int


def frame_pixels(data: bytes, width: int) -> tuple[int, ...]:
    """Unpack a little-endian RGB565 frame of the given width."""
    assert len(data) % (2 * width) == 0
    return struct.unpack(f"<{len(data) // 2}H", data)


def is_line(pixels: tuple[int, ...], area: ListArea, y: int, colour: int) -> bool:
    """Return whether line y is that colour across the area's edge columns."""
    return all(pixels[y * area.width + x] == colour for x in range(area.edge[0], area.edge[1]))


def boundaries(pixels: tuple[int, ...], area: ListArea) -> list[int]:
    """Return the first line of each row below a divider (or divider pair) in the area."""
    found = []
    previous = False
    for y in range(area.top, area.bottom):
        line = is_line(pixels, area, y, area.divider)
        if previous and not line:
            found.append(y)
        previous = line
    return found


def highlight_faults(pixels: tuple[int, ...], area: ListArea) -> list[str]:
    """List highlight runs that a background line separates from the line above or the bar."""
    lit = [y for y in range(area.top, area.bottom) if is_line(pixels, area, y, area.highlight)]
    faults = []
    for first, last in zip(
        [y for y in lit if y - 1 not in lit], [y for y in lit if y + 1 not in lit], strict=True
    ):
        if first > area.top and not is_line(pixels, area, first - 1, area.divider):
            faults.append(f"selection {first}..{last} leaves a line above it")
        if last == area.bottom - 2 and not is_line(pixels, area, last + 1, area.divider):
            faults.append(f"selection {first}..{last} leaves a line above the bar's line")
    return faults


def row_faults(pixels: tuple[int, ...], area: ListArea) -> list[str]:
    """List doubled bottom lines, detached selections and part rows of less than p/4."""
    pitch = area.pitch
    rows = boundaries(pixels, area)
    faults = [
        f"rows {upper}..{lower} are not one pitch apart"
        for upper, lower in pairwise(rows)
        if lower - upper != pitch
    ]
    if is_line(pixels, area, area.bottom - 1, area.divider):
        faults.append(f"divider at {area.bottom - 1} doubles the bar's line")
    faults += highlight_faults(pixels, area)
    if not rows:
        return faults
    above = rows[0] - area.top
    if above > pitch:
        faults.append(f"{above - pitch} px above the first whole row")
    elif above < pitch and (4 * above < pitch or 4 * (pitch - above) < pitch):
        faults.append(f"top part row shows {above} of {pitch} px")
    # The row from the last boundary on is whole when only its divider lies
    # under the bar's line; one with more lines shown is the last row, which
    # has no divider of its own.
    below = area.bottom - rows[-1]
    if below >= pitch - 1:
        return faults
    if 4 * (pitch - below) < pitch:
        faults.append(f"bottom part row hides {pitch - below} of {pitch} px")
    elif 4 * below < pitch:
        faults.append(f"bottom part row shows {below} of {pitch} px")
    return faults


def scroll_step(before: tuple[int, ...], after: tuple[int, ...], area: ListArea) -> int:
    """Return the least row shift, modulo the pitch, between two frames' boundaries."""
    first, second = boundaries(before, area), boundaries(after, area)
    assert first, "the earlier frame shows no row divider"
    assert second, "the later frame shows no row divider"
    shift = (second[0] - first[0]) % area.pitch
    return min(shift, area.pitch - shift)


def sequence_faults(frames: dict[str, tuple[int, ...]], area: ListArea) -> list[str]:
    """List row faults in each named frame and scroll steps below p/4 between neighbours."""
    faults = [
        f"{name}: {fault}" for name, pixels in frames.items() for fault in row_faults(pixels, area)
    ]
    for previous, name in pairwise(frames):
        step = scroll_step(frames[previous], frames[name], area)
        if step > 0 and 4 * step < area.pitch:
            faults.append(f"{previous} -> {name}: scroll step {step} of {area.pitch} px")
    return faults
