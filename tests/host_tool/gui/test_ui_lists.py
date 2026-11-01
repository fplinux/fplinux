# SPDX-License-Identifier: GPL-2.0-only
"""List row and overflow geometry with the production view and real LVGL."""

from __future__ import annotations

import os
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.gui.list_rows import ListArea, frame_pixels, is_line, row_faults
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from types import ModuleType


@pytest.fixture(scope="module")
def ui_lists(
    tmp_path_factory: pytest.TempPathFactory,
    lvgl_library: tuple[Path, Path],
    ui_host_inputs: Path,
) -> Iterator[Path]:
    """Replace the application context and marquee, retaining native layout/scroll."""
    headers, archive = lvgl_library
    with c_build_directory(tmp_path_factory, "ui-lists") as directory:
        executable = directory / "ui-lists"
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
                str(ROOT / "tests/host_tool/gui/fplinux-ui-lists.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-listview.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-style.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-screen-model.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
                *(str(path) for path in sorted((ui_host_inputs / "fonts").glob("*.c"))),
                str(archive),
                "-lm",
            ],
            name="link production lists with the native LVGL layout and scrollbar",
        )
        yield executable


# "#" is the marker's ink and "." the row's own colour under it.
CHECK_BOXES = {
    16: (
        "################",
        "################",
        "##............##",
        "##............##",
        "##..........#.##",
        "##.........##.##",
        "##........##..##",
        "##.#.....##...##",
        "##.##...##....##",
        "##..##.##.....##",
        "##...###......##",
        "##....#.......##",
        "##............##",
        "##............##",
        "################",
        "################",
    ),
    20: (
        "####################",
        "####################",
        "##................##",
        "##................##",
        "##................##",
        "##.............#..##",
        "##............##..##",
        "##...........##...##",
        "##..........##....##",
        "##.........##.....##",
        "##..#.....##......##",
        "##..##...##.......##",
        "##...##.##........##",
        "##....###.........##",
        "##.....#..........##",
        "##................##",
        "##................##",
        "##................##",
        "####################",
        "####################",
    ),
    24: (
        "########################",
        "########################",
        "##....................##",
        "##....................##",
        "##....................##",
        "##....................##",
        "##.................#..##",
        "##................##..##",
        "##...............##...##",
        "##..............##....##",
        "##.............##.....##",
        "##..#.........##......##",
        "##..##.......##.......##",
        "##...##.....##........##",
        "##....##...##.........##",
        "##.....##.##..........##",
        "##......###...........##",
        "##.......#............##",
        "##....................##",
        "##....................##",
        "##....................##",
        "##....................##",
        "########################",
        "########################",
    ),
}


@dataclass(frozen=True)
class RowCells:
    """Independent row pitch, marker box and its offset in the cell, odd pixel below."""

    pitch: int
    padding: int
    marker: int
    offset: int


@pytest.mark.parametrize(
    ("screen", "cells"),
    [
        ((128, 128), RowCells(24, 4, 16, 3)),
        ((128, 160), RowCells(23, 4, 16, 3)),
        ((176, 220), RowCells(27, 6, 20, 3)),
        ((240, 320), RowCells(33, 8, 24, 4)),
        ((320, 240), RowCells(32, 6, 24, 3)),
    ],
    ids=["128x128", "128x160", "176x220", "240x320", "320x240"],
)
@pytest.mark.parametrize(
    "palette",
    [
        # background, foreground, accent, accent text, divider
        ("light", 0xF7BE, 0x18C3, 0x3B0E, 0xFFFF, 0xBE39),
        ("dark", 0x1082, 0xDEFB, 0xAE7B, 0x09A8, 0x3A29),
    ],
    ids=["light", "dark"],
)
def test_check_rows_centre_crisp_marks_in_cells_between_dividers(
    ui_lists: Path,
    tmp_path: Path,
    screen: tuple[int, int],
    cells: RowCells,
    palette: tuple[str, int, int, int, int, int],
) -> None:
    """Rendered rows invert only the cell above their divider, around a centred crisp mark.

    The marker keeps the same space above and below in a selected and an
    unselected cell, and its check is whole pixels in 45-degree steps.
    """
    theme, background, foreground, accent, accent_text, divider = palette
    width, height = screen
    output = tmp_path / "markers.rgb565"
    run_process(
        [str(ui_lists), "markers", str(width), str(height), theme, str(output)],
        name=f"render native check rows at {width}x{height} ({theme})",
        timeout=10,
        check=True,
    )
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        retained = Path(log_root) / f"ui-list-markers-{width}x{height}-{theme}"
        retained.mkdir(parents=True, exist_ok=True)
        shutil.copy2(output, retained)
    data = output.read_bytes()
    assert len(data) == 2 * width * height * 2
    marked = (True, False, True)
    for selected in (1, 2):
        frame = data[(selected - 1) * width * height * 2 : selected * width * height * 2]
        wrong = []
        for row in range(3):
            top = row * cells.pitch
            ink, cell = (accent_text, accent) if row == selected else (foreground, background)
            below = divider if row < 2 else background
            box = CHECK_BOXES[cells.marker]
            for y in range(top, top + cells.pitch):
                for x in [*range(cells.padding + cells.marker + 1), width - 1]:
                    expected = cell if y < top + cells.pitch - 1 else below
                    inside_y = y - top - cells.offset
                    inside_x = x - cells.padding
                    if 0 <= inside_y < cells.marker and 0 <= inside_x < cells.marker:
                        mark = box[inside_y][inside_x]
                        outline = min(
                            inside_x, inside_y, cells.marker - 1 - max(inside_x, inside_y)
                        )
                        if mark == "#" and (marked[row] or outline < 2):
                            expected = ink
                    actual = struct.unpack_from("<H", frame, 2 * (y * width + x))[0]
                    if actual != expected:
                        wrong.append((row, x, y, hex(actual), hex(expected)))
        assert not wrong, f"row {selected} selected: {len(wrong)} pixels differ, first {wrong[:8]}"


@pytest.mark.parametrize(
    "screen",
    # width, height and row pitch
    [(128, 128, 24), (128, 160, 23), (176, 220, 27), (240, 320, 33), (320, 240, 32)],
    ids=["128x128", "128x160", "176x220", "240x320", "320x240"],
)
@pytest.mark.parametrize(
    "palette",
    [
        # theme, divider, accent
        ("light", 0xBE39, 0x3B0E),
        ("dark", 0x3A29, 0xAE7B),
    ],
    ids=["light", "dark"],
)
@pytest.mark.parametrize("presentation", ["stored", "virtual"])
def test_list_ending_on_a_bar_shows_its_line_once_with_the_selection_touching_it(
    ui_lists: Path,
    tmp_path: Path,
    screen: tuple[int, int, int],
    palette: tuple[str, int, int],
    presentation: str,
) -> None:
    """Three rows end on a bar's line: one line, and the selection meets it.

    The page's bottom row at the top of the list, the next page's bottom row
    and the last row each have their cell between the line above them and
    the bar's line, with no doubled divider or background line next to it.
    """
    theme, divider, accent = palette
    width, height, pitch = screen
    output = tmp_path / "bar-line.rgb565"
    result = run_process(
        [str(ui_lists), f"bar-line-{presentation}", str(width), str(height), theme, str(output)],
        name=f"render native {presentation} rows ending on a bar at {width}x{height} ({theme})",
        timeout=10,
    )
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        retained = Path(log_root) / f"ui-list-bar-line-{presentation}-{width}x{height}-{theme}"
        retained.mkdir(parents=True, exist_ok=True)
        shutil.copy2(output, retained)
    # A title band's line at y 0, then three rows of the pitch, the last
    # one's divider at the bar's line.
    bar = 3 * pitch
    area = ListArea(
        width=width, top=1, bottom=bar, pitch=pitch, edge=(0, 2), divider=divider, highlight=accent
    )
    data = output.read_bytes()
    assert len(data) == 4 * width * height * 2
    for index, name in enumerate(("first page", "next page", "last row")):
        pixels = frame_pixels(
            data[index * width * height * 2 : (index + 1) * width * height * 2], width
        )
        assert is_line(pixels, area, bar, divider), name
        assert row_faults(pixels, area) == [], name
        assert [y for y in range(1, bar) if is_line(pixels, area, y, accent)] == list(
            range(bar - pitch + 1, bar)
        ), name
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize(
    "screen",
    # width, height and row pitch
    [(128, 128, 24), (128, 160, 23), (176, 220, 27), (240, 320, 33), (320, 240, 32)],
    ids=["128x128", "128x160", "176x220", "240x320", "320x240"],
)
@pytest.mark.parametrize(
    "palette",
    [
        # theme, border, focus
        ("light", 0xBE39, 0x3B0E),
        ("dark", 0x3A29, 0xAE7B),
    ],
    ids=["light", "dark"],
)
@pytest.mark.parametrize("presentation", ["stored", "virtual"])
def test_overflowing_list_fills_its_scroll_channel_between_the_lines_around_it(
    ui_lists: Path,
    tmp_path: Path,
    screen: tuple[int, int, int],
    palette: tuple[str, int, int],
    presentation: str,
) -> None:
    """Ten rows fill the right four columns with the border colour; two rows leave none.

    At the top, in the middle and at the end of the list the channel runs
    from the title line to the bar's line with the focus thumb inside it,
    touching the title line first and the bar's line last.
    Once the rows fit, the right edge shows the same rows as the left edge.
    """
    theme, border, focus = palette
    width, height, pitch = screen
    output = tmp_path / "bar-line.rgb565"
    run_process(
        [str(ui_lists), f"bar-line-{presentation}", str(width), str(height), theme, str(output)],
        name=f"render native {presentation} list scroll channel at {width}x{height} ({theme})",
        timeout=10,
        check=True,
    )
    # The title band's line at y 0 and the bar's line three rows below it.
    bar = 3 * pitch
    data = output.read_bytes()
    assert len(data) == 4 * width * height * 2
    frames = [
        frame_pixels(data[index * width * height * 2 : (index + 1) * width * height * 2], width)
        for index in range(4)
    ]
    for name, pixels in zip(("first page", "next page", "last row"), frames[:3], strict=True):
        column = [[pixels[y * width + x] for y in range(1, bar)] for x in range(width - 5, width)]
        assert column[0] == [pixels[y * width] for y in range(1, bar)], name
        assert column[1] == column[4] == [border] * (bar - 1), name
        assert column[2] == column[3], name
        assert set(column[2]) == {border, focus}, name
        assert pixels[bar * width + width - 3 : (bar + 1) * width] == (border,) * 3, name
    # The thumb's track is the channel: it touches the title line at the
    # start and the bar's line at the end, never covering either.
    assert frames[0][1 * width + width - 3] == focus
    assert frames[2][(bar - 1) * width + width - 3] == focus
    fitted = frames[3]
    for x in range(width - 4, width):
        assert [fitted[y * width + x] for y in range(1, bar)] == [
            fitted[y * width] for y in range(1, bar)
        ], f"fitted rows at column {x}"


@pytest.mark.parametrize("screen", [(128, 160), (240, 320), (320, 240)])
def test_stored_status_row_keeps_emphasis_detail_and_borrowed_icon_inside_viewport(
    ui_lists: Path, screen: tuple[int, int]
) -> None:
    width, height = screen
    run_process(
        [str(ui_lists), "status-row", str(width), str(height), "0", "0"],
        name=f"native emphasized status row at {width}x{height}",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize(
    ("width", "height", "pitch"),
    [(128, 128, 36), (128, 160, 35), (176, 220, 38), (240, 320, 43), (320, 240, 43)],
)
@pytest.mark.parametrize("theme", ["light", "dark"])
def test_stored_controls_render_levels_and_values_at_each_profile(
    ui_lists: Path, width: int, height: int, pitch: int, theme: str
) -> None:
    run_process(
        [str(ui_lists), "controls", str(width), str(height), theme, str(pitch)],
        name=f"native list controls at {width}x{height} ({theme})",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize(
    ("width", "height", "row_height", "padding"),
    [
        (128, 128, 24, 4),
        (128, 160, 23, 4),
        (176, 220, 27, 6),
        (240, 320, 33, 8),
        (320, 240, 32, 6),
    ],
)
def test_native_scrollbar_keeps_its_thumb_in_a_channel_and_hides_both_when_content_fits(
    ui_lists: Path, width: int, height: int, row_height: int, padding: int
) -> None:
    """A list and raw viewports, bare and bordered, grow, scroll and shrink.

    While content overflows, the idle thumb lies one pixel from the right of
    a four-pixel channel in the border colour over the height inside any
    border. At the ends of the scroll it reaches a border there and
    otherwise stops a pixel short, as no line lies outside these viewports;
    content that fits shows neither.
    """
    run_process(
        [str(ui_lists), "scrollbar", str(width), str(height), str(row_height), str(padding)],
        name=f"native shared scrollbar appearance at {width}x{height}",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize(
    ("presentation", "row_height"), [("icon", 23), ("detail", 35), ("inline", 23), ("virtual", 35)]
)
def test_list_keypad_keeps_decorated_and_virtual_selection_visible(
    ui_lists: Path, presentation: str, row_height: int
) -> None:
    """Rows retain their text/detail/icon contents while native scrolling reveals focus."""
    run_process(
        [str(ui_lists), presentation, "128", "160", str(row_height), "4"],
        name=f"native {presentation} list keypad selection",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("presentation", ["stored", "virtual"])
def test_native_list_omits_only_actual_final_divider_after_row_changes(
    ui_lists: Path, presentation: str
) -> None:
    """Append, insert, remove and refill update native borders without shortening focus."""
    run_process(
        [str(ui_lists), f"final-divider-{presentation}", "128", "160", "0", "0"],
        name=f"native {presentation} final divider after row changes",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("operation", ["set", "append", "insert"])
@pytest.mark.parametrize("size", [(128, 160), (240, 320), (320, 240)])
def test_first_detail_keeps_same_selected_row_fully_visible(
    ui_lists: Path, operation: str, size: tuple[int, int]
) -> None:
    """A larger row pitch retains focus, including its shifted index after insert."""
    width, height = size
    run_process(
        [str(ui_lists), f"first-detail-{operation}", str(width), str(height), "0", "0"],
        name=f"native first detail through {operation} at {width}x{height}",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("operation", ["set", "append"])
def test_first_detail_keeps_read_only_list_without_focus_or_forced_scroll(
    ui_lists: Path, operation: str
) -> None:
    run_process(
        [str(ui_lists), f"first-detail-read-only-{operation}", "128", "160", "0", "0"],
        name=f"native read-only first detail through {operation}",
        timeout=10,
        check=True,
    )


def test_list_text_update_without_new_detail_preserves_scroll(ui_lists: Path) -> None:
    run_process(
        [str(ui_lists), "text-update", "128", "160", "0", "0"],
        name="native text-only list update preserves scroll",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("presentation", ["stored", "virtual"])
@pytest.mark.parametrize("viewport", ["full", "one-row"])
def test_native_list_arrows_wrap_but_pages_clamp(
    ui_lists: Path, presentation: str, viewport: str
) -> None:
    """Initial and repeated row steps wrap; even a one-row page keeps its bounds."""
    run_process(
        [str(ui_lists), f"navigation-{presentation}-{viewport}", "128", "160", "0", "0"],
        name=f"native {presentation} selection with a {viewport} viewport",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("presentation", ["stored", "virtual"])
def test_native_list_first_visible_uses_scroll_and_whole_row_geometry(
    ui_lists: Path, presentation: str
) -> None:
    run_process(
        [str(ui_lists), f"first-visible-{presentation}", "240", "320", "0", "0"],
        name=f"native {presentation} list first visible row",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("presentation", ["stored", "virtual"])
def test_native_list_set_first_visible_restores_view_and_keeps_focus_visible(
    ui_lists: Path, presentation: str
) -> None:
    """A saved first row returns, clamps to the content and never hides the focus."""
    run_process(
        [str(ui_lists), f"set-first-visible-{presentation}", "240", "320", "0", "0"],
        name=f"native {presentation} list restores its first visible row",
        timeout=10,
        check=True,
    )


# Five texts made 1 and 2 px too wide, then one at exactly its own width.
ELLIPSIS_TEXTS = {
    "ru": ("Настройки экрана", "Подключения", "Мои заметки", "Карандаш", "Пейзаж", "Сохранить"),
    "en": ("Display settings", "Information", "My notes", "Keyboard", "Calculator", "Bluetooth"),
}


@pytest.mark.parametrize("profile", ["128x128", "128x160", "176x220", "240x320", "320x240"])
@pytest.mark.parametrize(
    ("role", "padding"), [("body", 0), ("body", 1), ("title", 0), ("legend", 0)]
)
@pytest.mark.parametrize("locale", ["ru", "en"])
def test_dotted_label_keeps_the_longest_start_that_fits_with_baseline_dots(  # noqa: PLR0913, PLR0917 -- independent profile, font role and text inputs.
    ui_lists: Path,
    tmp_path: Path,
    rgb565_png: ModuleType,
    profile: str,
    role: str,
    padding: int,
    locale: str,
) -> None:
    """A text 1-2 px too wide keeps every letter that fits before three baseline dots.

    The candidates are every start of the text without trailing spaces
    followed by "...", measured by the font; the label must show the longest
    one within its box, and a text that fits stays whole.
    """
    texts = ELLIPSIS_TEXTS[locale]
    cases = [(text, over) for text in texts[:5] for over in (1, 2)]
    cases.append((texts[5], 0))
    (tmp_path / "cases.tsv").write_text(
        "".join(f"{role}\t{padding}\t{over}\t{text}\n" for text, over in cases), encoding="utf-8"
    )
    run_process(
        [str(ui_lists), "ellipsis", "240", "320", profile, str(tmp_path)],
        name=f"render dotted {role} labels of {profile} ({locale})",
        timeout=10,
        check=True,
    )
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        retained = Path(log_root) / f"ui-ellipsis-{profile}-{role}-{padding}-{locale}"
        retained.mkdir(parents=True, exist_ok=True)
        rgb565_png.convert(240, 320, tmp_path / "ellipsis.rgb565", retained / "ellipsis.png")
        shutil.copy2(tmp_path / "results.tsv", retained)
    pixels = frame_pixels((tmp_path / "ellipsis.rgb565").read_bytes(), 240)
    rows = (tmp_path / "results.tsv").read_text(encoding="utf-8").splitlines()
    assert len(rows) == len(cases)
    for (text, over), row in zip(cases, rows, strict=True):
        fields = row.split("\t")
        natural, box, left, top, baseline, line = (int(fields[i]) for i in (0, 1, 3, 4, 5, 6))
        shown = fields[2]
        candidates = [int(width) for width in fields[7].split(",")]
        assert natural - box == over
        if over <= 0:
            assert shown == text
            continue
        fitting = [index for index, width in enumerate(candidates) if width <= box]
        kept = max(fitting, default=0)
        assert shown == text[:kept].rstrip(" ") + "...", (text, box)
        # Ink stays in the box, and the dots' columns hold ink only near the baseline.
        ink = {
            (x, y)
            for y in range(top, top + line)
            for x in range(240)
            if pixels[y * 240 + x] != 0xFFFF
        }
        assert all(x < left + box for x, _ in ink), (text, box)
        dots = range(left + candidates[kept] - candidates[0] + 1, left + candidates[kept])
        heights = sorted({y - top for x, y in ink if x in dots})
        assert heights, (text, shown)
        # The lowest row of ink rests on the baseline, give or take a row of
        # antialiasing; no dot reaches the middle of the letters.
        assert baseline - 2 <= heights[-1] <= baseline, (text, heights, baseline)
        assert heights[0] >= baseline - 5, (text, heights, baseline)
