# SPDX-License-Identifier: GPL-2.0-only
"""List row and overflow geometry with the production view and real LVGL."""

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
def ui_lists(
    tmp_path_factory: pytest.TempPathFactory, lvgl_library: tuple[Path, Path]
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
                str(archive),
                "-lm",
            ],
            name="link production lists with the native LVGL layout and scrollbar",
        )
        yield executable


@pytest.mark.parametrize(
    ("width", "height", "row_height", "padding"),
    [
        (128, 128, 20, 4),
        (128, 160, 22, 4),
        (176, 220, 26, 6),
        (240, 320, 36, 8),
        (320, 240, 30, 6),
    ],
)
def test_list_rows_reserve_scrollbar_space_only_while_content_overflows(
    ui_lists: Path, width: int, height: int, row_height: int, padding: int
) -> None:
    """Growing adds the four-pixel lane; shrinking restores the whole width."""
    run_process(
        [str(ui_lists), "geometry", str(width), str(height), str(row_height), str(padding)],
        name=f"native list geometry at {width}x{height}",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize(
    ("width", "height", "row_height", "padding"),
    [
        (128, 128, 20, 4),
        (128, 160, 22, 4),
        (176, 220, 26, 6),
        (240, 320, 36, 8),
        (320, 240, 30, 6),
    ],
)
def test_native_scrollbar_keeps_one_pixel_edge_insets_and_hides_when_content_fits(
    ui_lists: Path, width: int, height: int, row_height: int, padding: int
) -> None:
    """The shared style keeps the same idle thumb after growing, scrolling and shrinking."""
    run_process(
        [str(ui_lists), "scrollbar", str(width), str(height), str(row_height), str(padding)],
        name=f"native shared scrollbar appearance at {width}x{height}",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize(
    ("presentation", "row_height"), [("icon", 22), ("detail", 32), ("inline", 22), ("virtual", 32)]
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
