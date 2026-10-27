# SPDX-License-Identifier: GPL-2.0-only
"""Complete Terminal captures with real UI, PTY and bitmap terminal cells."""

from __future__ import annotations

import json
import os
import re
import shutil
from pathlib import Path
from typing import TYPE_CHECKING, TypedDict, cast

import pytest

from tests import ROOT
from tests.host_tool.gui.png_capture import png_pixels
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


class LabelObservation(TypedDict):
    """Text and geometry read from a visible LVGL label after painting."""

    text: str
    bounds: list[int]
    text_size: list[int]
    scrolls: bool
    ellipsizes: bool


class FrameObservation(TypedDict):
    """The actual Canvas content area and visible text of one committed frame."""

    content: list[int]
    labels_truncated: bool
    labels: list[LabelObservation]
    shell_resources_closed: bool
    pty_watched: bool
    final_output_visible: bool
    terminal: TerminalObservation


class TerminalObservation(TypedDict):
    """Read-only VT state associated with the painted terminal canvas."""

    columns: int
    rows: int
    cursor_x: int
    cursor_y: int
    flags: int
    preedit: int
    first_character: int
    first_background: int
    edge_character: int
    cursor_visible: bool


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
        timeout=360,
    )
    (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(output, Path(log_root) / "terminal-screens", dirs_exist_ok=True)
    try:
        assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
        yield output, result.stdout
    finally:
        shutil.rmtree(output)


def assert_preedit_ink_inside_highlight(
    width: int,
    pixels: bytes,
    frame: FrameObservation,
    cell: tuple[int, int],
    theme: str = "dark",
) -> None:
    """Check that the pending letter's ink stays inside its highlighted cell."""
    accent = b"\xad\xcf\xde" if theme == "dark" else b"\x39\x61\x73"
    accent_text = b"\x08\x34\x42" if theme == "dark" else b"\xff\xff\xff"
    terminal = frame["terminal"]
    column, row = terminal["cursor_x"], terminal["cursor_y"]
    assert terminal["preedit"]
    assert column < terminal["columns"]
    assert row < terminal["rows"]
    content_left, content_top, content_width, content_height = frame["content"]
    cell_width, cell_height = cell
    grid_left = content_left + (content_width - terminal["columns"] * cell_width) // 2
    grid_top = content_top + (content_height - terminal["rows"] * cell_height) // 2
    left = grid_left + column * cell_width
    top = grid_top + row * cell_height
    right, bottom = left + cell_width, top + cell_height
    assert right <= content_left + content_width
    assert bottom <= content_top + content_height
    actual = terminal_cell(width, pixels, frame["content"], cell, column, row)
    assert set(actual) == {accent, accent_text}
    for y in range(content_top, content_top + content_height):
        for x in range(content_left, content_left + content_width):
            offset = (y * width + x) * 3
            if pixels[offset : offset + 3] == accent:
                assert left <= x < right
                assert top <= y < bottom


# Keep pixel stride, canvas bounds, PSF dimensions and both cell coordinates explicit.
def terminal_cell(  # noqa: PLR0913, PLR0917
    width: int,
    pixels: bytes,
    content: list[int],
    cell: tuple[int, int],
    column: int,
    row: int,
) -> list[bytes]:
    """Crop an actual PSF cell without deriving its colours or shape from the renderer."""
    left, top = content[:2]
    cell_width, cell_height = cell
    left += (content[2] % cell_width) // 2
    top += (content[3] % cell_height) // 2
    return [
        pixels[
            ((top + row * cell_height + y) * width + left + column * cell_width + x) * 3 : (
                (top + row * cell_height + y) * width + left + column * cell_width + x + 1
            )
            * 3
        ]
        for y in range(cell_height)
        for x in range(cell_width)
    ]


def terminal_screen_state(frame: FrameObservation) -> tuple[int, int, int, int, int, int]:
    """State that a local preview must preserve while changing only displayed pixels."""
    terminal = frame["terminal"]
    return (
        terminal["columns"],
        terminal["rows"],
        terminal["cursor_x"],
        terminal["cursor_y"],
        terminal["flags"],
        terminal["edge_character"],
    )


def test_actual_idle_terminal_sleeps_between_cursor_deadlines(
    terminal_screens: tuple[Path, str],
) -> None:
    _, transcript = terminal_screens
    samples = re.findall(
        r"terminal-idle ([a-z-]+): elapsed_ms=(\d+) cpu_us=(\d+) "
        r"poll_wakeups=(\d+) timeout_wakeups=(\d+) terminal_ticks=(\d+)",
        transcript,
    )
    assert [sample[0] for sample in samples] == ["visible-cursor", "hidden"]
    for state, elapsed, _cpu, _wakeups, timeouts, ticks in samples:
        assert 1200 <= int(elapsed) < 3000
        if state == "visible-cursor":
            assert 2 <= int(ticks) <= 4
        else:
            assert int(ticks) == 0
        assert int(timeouts) < 20


# Capture axes and literal expected colours stay independent; pytest supplies them by name.
@pytest.mark.parametrize("suffix", ["-128x128", "-128x160", "-176x220", "", "-320x240"])
@pytest.mark.parametrize("locale", ["en", "ru"])
@pytest.mark.parametrize(
    ("theme", "accent", "ink"),
    [("dark", b"\xad\xcf\xde", b"\x08\x34\x42"), ("light", b"\x39\x61\x73", b"\xff\xff\xff")],
)
@pytest.mark.parametrize(("font", "cell"), [("6x12", (6, 12)), ("8x16", (8, 16))])
def test_actual_pending_glyph_uses_the_active_pair_without_hiding_strokes(  # noqa: PLR0913
    *,
    terminal_screens: tuple[Path, str],
    suffix: str,
    locale: str,
    theme: str,
    accent: bytes,
    ink: bytes,
    font: str,
    cell: tuple[int, int],
) -> None:
    output, _ = terminal_screens
    prefix = f"terminal-render-{locale}-{theme}-{font}"
    reference_name = f"{prefix}-reference{suffix}"
    frame = cast("FrameObservation", json.loads((output / f"{reference_name}.json").read_text()))
    width, _, reference = png_pixels(output / f"{reference_name}.png")
    for column, (state, character) in enumerate(
        [("g", "g"), ("p", "p"), ("q", "q"), ("y", "y"), ("underscore", "_")]
    ):
        name = f"{prefix}-pending-{state}{suffix}"
        pending_frame = cast("FrameObservation", json.loads((output / f"{name}.json").read_text()))
        assert pending_frame["terminal"]["preedit"] == ord(character)
        assert pending_frame["terminal"]["cursor_x"] == 0
        assert pending_frame["terminal"]["cursor_y"] == 1
        _, _, pixels = png_pixels(output / f"{name}.png")
        shape = terminal_cell(width, reference, frame["content"], cell, column, 0)
        pending = terminal_cell(width, pixels, frame["content"], cell, 0, 1)
        assert b"\xde\xdf\xde" in shape
        assert pending == [ink if pixel == b"\xde\xdf\xde" else accent for pixel in shape], name


# Explicit profile, locale, theme and font axes identify each independently captured frame.
@pytest.mark.parametrize("suffix", ["-128x128", "-128x160", "-176x220", "", "-320x240"])
@pytest.mark.parametrize("locale", ["en", "ru"])
@pytest.mark.parametrize("theme", ["dark", "light"])
@pytest.mark.parametrize(("font", "cell"), [("6x12", (6, 12)), ("8x16", (8, 16))])
def test_actual_cursor_stays_visible_on_resolved_inverse_and_white_backgrounds(  # noqa: PLR0913
    *,
    terminal_screens: tuple[Path, str],
    suffix: str,
    locale: str,
    theme: str,
    font: str,
    cell: tuple[int, int],
) -> None:
    output, _ = terminal_screens
    prefix = f"terminal-render-{locale}-{theme}-{font}"
    for state, background in [("inverse", b"\xde\xdf\xde"), ("white", b"\xff\xff\xff")]:
        name = f"{prefix}-{state}-cursor{suffix}"
        frame = cast("FrameObservation", json.loads((output / f"{name}.json").read_text()))
        assert frame["terminal"]["cursor_visible"]
        assert frame["terminal"]["preedit"] == 0
        width, _, pixels = png_pixels(output / f"{name}.png")
        actual = terminal_cell(width, pixels, frame["content"], cell, 0, 0)
        assert actual[: -2 * cell[0]] == [background] * ((cell[1] - 2) * cell[0])
        assert actual[-2 * cell[0] :] == [b"\x10\x10\x10"] * (2 * cell[0]), name


# Explicit profile, locale, theme and font axes identify each independently captured frame.
@pytest.mark.parametrize("suffix", ["-128x128", "-128x160", "-176x220", "", "-320x240"])
@pytest.mark.parametrize("locale", ["en", "ru"])
@pytest.mark.parametrize("theme", ["dark", "light"])
@pytest.mark.parametrize(("font", "cell"), [("6x12", (6, 12)), ("8x16", (8, 16))])
def test_actual_deferred_wrap_previews_the_next_cell_and_cancel_restores_the_view(  # noqa: PLR0913
    *,
    terminal_screens: tuple[Path, str],
    suffix: str,
    locale: str,
    theme: str,
    font: str,
    cell: tuple[int, int],
) -> None:
    output, _ = terminal_screens
    prefix = f"terminal-render-{locale}-{theme}-{font}"
    reference_name = f"{prefix}-reference{suffix}"
    reference_frame = cast(
        "FrameObservation", json.loads((output / f"{reference_name}.json").read_text())
    )
    width, _, reference = png_pixels(output / f"{reference_name}.png")
    edge_glyph = terminal_cell(width, reference, reference_frame["content"], cell, 5, 0)
    for state in ["edge", "bottom"]:
        frames: dict[str, FrameObservation] = {}
        images: dict[str, bytes] = {}
        for phase in ["before", "pending", "cancel"]:
            name = f"{prefix}-{state}-{phase}{suffix}"
            frames[phase] = cast(
                "FrameObservation", json.loads((output / f"{name}.json").read_text())
            )
            images[phase] = png_pixels(output / f"{name}.png")[2]
        before = frames["before"]
        columns, rows = before["terminal"]["columns"], before["terminal"]["rows"]
        source_row = rows - 1 if state == "bottom" else 0
        assert before["terminal"]["cursor_x"] == columns
        assert before["terminal"]["cursor_y"] == source_row
        marker = terminal_cell(
            width, images["before"], before["content"], cell, columns - 1, source_row
        )
        assert marker[-2 * cell[0] :] == [b"\xde\xdf\xde"] * (2 * cell[0])
        pending = frames["pending"]
        assert pending["terminal"]["preedit"] == ord("a")
        assert terminal_screen_state(pending) == terminal_screen_state(before)
        assert terminal_screen_state(frames["cancel"]) == terminal_screen_state(before)
        preview_row = rows - 1 if state == "bottom" else 1
        preview = terminal_cell(width, images["pending"], pending["content"], cell, 0, preview_row)
        accent = b"\xad\xcf\xde" if theme == "dark" else b"\x39\x61\x73"
        assert accent in preview, (prefix, state, "pending preview is missing")
        retained_row = rows - 2 if state == "bottom" else 0
        assert (
            terminal_cell(
                width, images["pending"], pending["content"], cell, columns - 1, retained_row
            )
            == edge_glyph
        )
        left, top, content_width, content_height = before["content"]
        for row in range(top, top + content_height):
            start = (row * width + left) * 3
            stop = start + content_width * 3
            assert images["cancel"][start:stop] == images["before"][start:stop]


@pytest.mark.parametrize(
    ("suffix", "size"),
    [
        ("-128x128", (128, 96)),
        ("-128x160", (128, 128)),
        ("-176x220", (176, 178)),
        ("", (240, 268)),
        ("-320x240", (320, 196)),
    ],
)
@pytest.mark.parametrize("locale", ["en", "ru"])
@pytest.mark.parametrize("theme", ["dark", "light"])
def test_actual_failed_shell_keeps_last_output_and_scrollback_after_releasing_pty(
    terminal_screens: tuple[Path, str],
    suffix: str,
    size: tuple[int, int],
    locale: str,
    theme: str,
) -> None:
    output, transcript = terminal_screens
    width, height = size
    assert (
        f"terminal-ended {width}x{height} {locale} {theme}: final output; "
        "no PTY resources/input; Scrollback and Close\n"
    ) in transcript
    if locale == "en" and theme == "dark":
        assert (
            f"terminal-keyboard {width}x{height}: live Esc reached PTY; stopped Esc Back/Close\n"
        ) in transcript
    for state in ("terminal-ended", "terminal-ended-history"):
        name = f"{state}-{locale}-{theme}{suffix}"
        frame = cast("FrameObservation", json.loads((output / f"{name}.json").read_text()))
        assert frame["shell_resources_closed"]
        assert not frame["pty_watched"]
        if state == "terminal-ended":
            assert frame["final_output_visible"]
            expected_status = (
                ("Сигнал 15" if locale == "ru" else "Signal 15")
                if theme == "light"
                else ("Код 37" if locale == "ru" else "Exit 37")
            )
            assert [label["text"] for label in frame["labels"]].count(expected_status) == 1
        assert all(
            "Ctrl" not in label["text"] and "C+A+S" not in label["text"]
            for label in frame["labels"]
        )
        for label in frame["labels"]:
            left, top, label_width, label_height = label["bounds"]
            assert 0 <= left < left + label_width <= width
            assert 0 <= top < top + label_height <= height
            assert label["text_size"][0] <= label_width
            assert label["text_size"][1] <= label_height
        assert png_pixels(output / f"{name}.png")[:2] == size


@pytest.mark.parametrize("size", [(128, 96), (128, 128), (176, 178), (240, 268), (320, 196)])
def test_actual_special_key_menu_sends_navigation_after_pending_text(
    terminal_screens: tuple[Path, str], size: tuple[int, int]
) -> None:
    _, transcript = terminal_screens
    assert (
        f"terminal-special {size[0]}x{size[1]}: "
        "Esc/Home/End/Delete/PageUp/PageDown reached the real PTY; pending text kept its order\n"
    ) in transcript


@pytest.mark.parametrize("size", [(128, 128), (240, 268), (320, 196)])
def test_actual_terminal_font_mark_stays_on_current_choice_until_ok(
    terminal_screens: tuple[Path, str], size: tuple[int, int]
) -> None:
    """Real list navigation and Back retain the applied font; OK resizes the PTY."""
    _, transcript = terminal_screens
    width, height = size
    assert (
        transcript.count(
            f"terminal-font-selection {width}x{height}: "
            "current mark survives focus and Back; OK applies\n"
        )
        == 4
    )


@pytest.mark.parametrize(
    ("suffix", "size", "layout", "other_layout"),
    [
        ("-128x160", (128, 128), ((21, 9), (6, 12)), ((16, 6), (8, 16))),
        ("", (240, 268), ((30, 15), (8, 16)), ((40, 20), (6, 12))),
        ("-320x240", (320, 196), ((40, 10), (8, 16)), ((53, 14), (6, 12))),
    ],
)
def test_actual_terminal_keeps_pty_cells_and_options_navigation(
    terminal_screens: tuple[Path, str],
    suffix: str,
    size: tuple[int, int],
    layout: tuple[tuple[int, int], tuple[int, int]],
    other_layout: tuple[tuple[int, int], tuple[int, int]],
) -> None:
    """Real TIOCGWINSZ and pending glyphs retain the supported terminal grids."""
    output, transcript = terminal_screens
    grid, cell = layout
    other_grid, other_cell = other_layout
    width, height = size
    columns, rows = grid
    assert transcript.count(f"terminal-pty default {width}x{height}: {columns}x{rows}\n") == 4
    other_columns, other_rows = other_grid
    assert (
        transcript.count(f"terminal-pty other {width}x{height}: {other_columns}x{other_rows}\n")
        == 4
    )
    states = []
    for state in ("terminal-keypad", "terminal-menu", "terminal-modifiers", "terminal-font-other"):
        actual_width, actual_height, pixels = png_pixels(output / f"{state}{suffix}.png")
        assert (actual_width, actual_height) == (width, height)
        assert f"{state}: {width}x{height}\n" in transcript
        assert len({pixels[index : index + 3] for index in range(0, len(pixels), 3)}) > 1
        frame = cast(
            "FrameObservation", json.loads((output / f"{state}{suffix}.json").read_text())
        )
        if state == "terminal-keypad":
            assert_preedit_ink_inside_highlight(actual_width, pixels, frame, cell)
        elif state == "terminal-font-other":
            assert_preedit_ink_inside_highlight(actual_width, pixels, frame, other_cell)
        states.append(pixels)
    assert len(set(states)) == 4


@pytest.mark.parametrize(
    "profile",
    [
        ("-128x160", (128, 128), [0, 20, 128, 108], 124),
        ("", (240, 268), [0, 28, 240, 240], 232),
        ("-320x240", (320, 196), [0, 26, 320, 170], 314),
    ],
)
@pytest.mark.parametrize(
    "language", [("en", "Terminal", "Scrollback"), ("ru", "Терминал", "Прокрутка")]
)
@pytest.mark.parametrize("theme", ["dark", "light"])
@pytest.mark.parametrize("terminal_font", [("6x12", (6, 12)), ("8x16", (8, 16))])
def test_actual_terminal_header_keeps_status_whole_and_restores_input_mode(
    terminal_screens: tuple[Path, str],
    profile: tuple[str, tuple[int, int], list[int], int],
    language: tuple[str, str, str],
    theme: str,
    terminal_font: tuple[str, tuple[int, int]],
) -> None:
    """Real localized frames use one static header throughout live and scrollback."""
    output, transcript = terminal_screens
    suffix, size, content, right_edge = profile
    locale, live_title, history_title = language
    font, cell = terminal_font
    width, height = size
    prefix = f"terminal-header-{locale}-{theme}-{font}"
    headers = {}
    for state, mode in (
        ("live", "abc"),
        ("armed", "abc"),
        ("upper", "ABC"),
        ("ru-upper", "АБВ"),
        ("ru-lower", "абв"),
        ("numeric", "123"),
        ("history-zero", "↑0"),
        ("history-max", "↑512"),
        ("live-after-history", "АБВ"),
    ):
        name = f"{prefix}-{state}{suffix}"
        frame = cast("FrameObservation", json.loads((output / f"{name}.json").read_text()))
        assert not frame["labels_truncated"]
        assert frame["content"] == content
        expected_title = history_title if state.startswith("history-") else live_title
        titles = [
            label
            for label in frame["labels"]
            if label["text"] == expected_title
            or (
                label["text"].endswith("...")
                and label["text"] != "..."
                and expected_title.startswith(label["text"][:-3])
            )
        ]
        assert len(titles) == 1, (name, frame)
        title = titles[0]
        full_status = mode if state == "live" else f"Ctrl+Alt+Shift  {mode}"
        compact_status = mode if state == "live" else f"C+A+S  {mode}"
        statuses = [
            label for label in frame["labels"] if label["text"] in (full_status, compact_status)
        ]
        assert len(statuses) == 1, (name, frame)
        status = statuses[0]
        if width == 128:
            assert status["text"] == compact_status
        elif width == 320:
            assert status["text"] == full_status
        left, top, status_width, status_height = status["bounds"]
        assert left + status_width == right_edge
        assert title["bounds"][0] + title["bounds"][2] <= left
        assert status["text_size"][0] <= status_width
        assert status["text_size"][1] <= status_height
        assert title["ellipsizes"]
        assert title["text_size"][0] <= title["bounds"][2]
        for label in frame["labels"]:
            label_left, label_top, label_width, label_height = label["bounds"]
            assert 0 <= label_left < label_left + label_width <= width
            assert 0 <= label_top < label_top + label_height <= content[1]
            assert not label["scrolls"]
        actual_width, actual_height, pixels = png_pixels(output / f"{name}.png")
        assert (actual_width, actual_height) == size
        assert f"{prefix}-{state}: {width}x{height}\n" in transcript
        background = bytes((33, 32, 33) if theme == "dark" else (239, 239, 239))
        status_pixels = [
            pixels[(row * width + column) * 3 : (row * width + column + 1) * 3]
            for row in range(top, top + status_height)
            for column in range(left, left + status_width)
        ]
        assert any(pixel != background for pixel in status_pixels)
        headers[state] = pixels[: width * content[1] * 3]
        if state == "live":
            assert_preedit_ink_inside_highlight(actual_width, pixels, frame, cell, theme)
    assert headers["ru-upper"] == headers["live-after-history"]
    assert headers["history-zero"] != headers["history-max"]
    special_name = f"{prefix}-special-modifiers{suffix}"
    special = cast("FrameObservation", json.loads((output / f"{special_name}.json").read_text()))
    assert not special["labels_truncated"]
    statuses = [
        label for label in special["labels"] if label["text"] in ("Ctrl+Alt+Shift", "C+A+S")
    ]
    assert len(statuses) == 1
    assert statuses[0]["text_size"][0] <= statuses[0]["bounds"][2]
    assert statuses[0]["text_size"][1] <= statuses[0]["bounds"][3]
    escape = [label for label in special["labels"] if label["text"] == "Esc"]
    home = [label for label in special["labels"] if label["text"] == "Home"]
    assert len(escape) == len(home) == 1
    assert escape[0]["bounds"][1] < home[0]["bounds"][1]


@pytest.mark.parametrize(
    "profile",
    [
        ("-128x128", (128, 96), (1, 0, 126, 96)),
        ("-128x160", (128, 128), (1, 16, 126, 96)),
        ("-176x220", (176, 178), (5, 7, 166, 164)),
        ("", (240, 268), (24, 44, 192, 180)),
        ("-320x240", (320, 196), (20, 33, 280, 130)),
    ],
)
@pytest.mark.parametrize("locale", ["en", "ru"])
@pytest.mark.parametrize("appearance", [("dark", (173, 207, 222)), ("light", (57, 97, 115))])
@pytest.mark.parametrize("font", ["6x12", "8x16"])
def test_actual_terminal_symbols_are_centered_in_application_area(
    terminal_screens: tuple[Path, str],
    profile: tuple[str, tuple[int, int], tuple[int, int, int, int]],
    locale: str,
    appearance: tuple[str, tuple[int, int, int]],
    font: str,
) -> None:
    """Real Symbols pixels stay centered for both locales, startup themes and fonts."""
    output, transcript = terminal_screens
    suffix, size, popup = profile
    theme, border = appearance
    state = f"terminal-header-{locale}-{theme}-{font}-symbols"
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
