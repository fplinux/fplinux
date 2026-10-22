# SPDX-License-Identifier: GPL-2.0-only
"""Complete Terminal captures with real UI, PTY, Terminus cells and Noto chrome."""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.host_tool.gui.png_capture import png_pixels
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


@pytest.fixture(scope="module")
def terminal_screens(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path
) -> Iterator[tuple[Path, str]]:
    """Run the maintained producer, replacing only the shell's output stream."""
    output = tmp_path_factory.mktemp("terminal-screens")
    result = run_process(
        [
            "sh",
            str(ROOT / "site/scripts/screens/render-terminal.sh"),
            str(output),
            str(ui_host_inputs),
        ],
        name="build and capture the actual Terminal application",
        timeout=240,
        check=True,
    )
    (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(output, Path(log_root) / "terminal-screens", dirs_exist_ok=True)
    try:
        yield output, result.stdout
    finally:
        shutil.rmtree(output)


def assert_preedit_ink_inside_highlight(width: int, pixels: bytes, cell: tuple[int, int]) -> None:
    """Check that the pending letter's ink stays inside its highlighted cell."""
    highlight = []
    ink = []
    for offset in range(0, len(pixels), 3):
        colour = pixels[offset : offset + 3]
        position = ((offset // 3) % width, (offset // 3) // width)
        if colour == bytes((173, 207, 222)):
            highlight.append(position)
        elif colour == bytes((8, 52, 66)):
            ink.append(position)
    assert highlight, "The preedit cell has no highlight."
    assert ink, "The preedit letter has no dark foreground pixels."
    left = min(x for x, _ in highlight)
    right = max(x for x, _ in highlight)
    top = min(y for _, y in highlight)
    bottom = max(y for _, y in highlight)
    assert (right - left + 1, bottom - top + 1) == cell
    outside = [(x, y) for x, y in ink if not (left <= x <= right and top <= y <= bottom)]
    assert not outside, f"Preedit ink outside cell {(left, top, right, bottom)}: {outside[:8]}"


@pytest.mark.parametrize(
    ("suffix", "size", "layout", "other_layout"),
    [
        ("-128x160", (128, 128), ((21, 9), (6, 10)), ((16, 6), (8, 14))),
        ("", (240, 268), ((30, 15), (8, 14)), ((40, 20), (6, 10))),
        ("-320x240", (320, 196), ((40, 10), (8, 14)), ((53, 14), (6, 10))),
    ],
)
def test_actual_terminal_capture_navigates_options_and_modifiers_at_each_profile(
    terminal_screens: tuple[Path, str],
    suffix: str,
    size: tuple[int, int],
    layout: tuple[tuple[int, int], tuple[int, int]],
    other_layout: tuple[tuple[int, int], tuple[int, int]],
) -> None:
    """Each integrated application area has drawn content and distinct real UI states."""
    output, transcript = terminal_screens
    grid, cell = layout
    other_grid, other_cell = other_layout
    width, height = size
    columns, rows = grid
    assert f"terminal-pty default {width}x{height}: {columns}x{rows}\n" in transcript
    other_columns, other_rows = other_grid
    assert f"terminal-pty other {width}x{height}: {other_columns}x{other_rows}\n" in transcript
    states = []
    for state in (
        "terminal-keypad",
        "terminal-menu",
        "terminal-modifiers",
        "terminal-font-other",
        "terminal-status",
    ):
        actual_width, actual_height, pixels = png_pixels(output / f"{state}{suffix}.png")
        assert (actual_width, actual_height) == (width, height)
        assert f"{state}: {width}x{height}\n" in transcript
        assert len({pixels[index : index + 3] for index in range(0, len(pixels), 3)}) > 8
        if state == "terminal-keypad":
            assert_preedit_ink_inside_highlight(actual_width, pixels, cell)
        elif state == "terminal-font-other":
            assert_preedit_ink_inside_highlight(actual_width, pixels, other_cell)
        states.append(pixels)
    assert len(set(states)) == 5
    assert (
        transcript.count(
            "input=ec; Options; Modifiers; Font size; history with all modifiers; close=ok\n"
        )
        == 3
    )


@pytest.mark.parametrize(
    "profile",
    [
        ("-128x160", (128, 128), (2, 16, 123, 96)),
        ("", (240, 268), (24, 44, 192, 180)),
        ("-320x240", (320, 196), (20, 33, 280, 130)),
    ],
)
@pytest.mark.parametrize(
    ("state", "border"),
    [
        ("terminal-symbols-star", (173, 207, 222)),
        ("terminal-symbols-menu", (173, 207, 222)),
        ("terminal-symbols-light", (57, 97, 115)),
        ("terminal-symbols-font-other", (173, 207, 222)),
    ],
)
def test_actual_terminal_symbols_are_centered_in_application_area(
    terminal_screens: tuple[Path, str],
    profile: tuple[str, tuple[int, int], tuple[int, int, int, int]],
    state: str,
    border: tuple[int, int, int],
) -> None:
    """Real Symbols pixels stay centered for both startup themes and terminal fonts."""
    output, transcript = terminal_screens
    suffix, size, popup = profile
    width, height, pixels = png_pixels(output / f"{state}{suffix}.png")
    assert (width, height) == size
    assert f"{state}: {width}x{height}\n" in transcript
    outline = [
        ((offset // 3) % width, (offset // 3) // width)
        for offset in range(0, len(pixels), 3)
        if pixels[offset : offset + 3] == bytes(border)
    ]
    assert outline, "Symbols has no visible focus border."
    left = min(x for x, _ in outline)
    top = min(y for _, y in outline)
    right = max(x for x, _ in outline)
    bottom = max(y for _, y in outline)
    assert (left, top, right - left + 1, bottom - top + 1) == popup
