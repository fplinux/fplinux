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
    ("width", "height", "margin", "offset"),
    [(128, 128, 4, 1), (128, 160, 4, 1), (176, 220, 6, 2), (240, 320, 8, 3), (320, 240, 6, 2)],
)
def test_list_rows_keep_symmetric_margins_with_and_without_overflow(
    ui_lists: Path, width: int, height: int, margin: int, offset: int
) -> None:
    """Selection width stays stable and the idle overflow thumb occupies the margin."""
    run_process(
        [str(ui_lists), "geometry", str(width), str(height), str(margin), str(offset)],
        name=f"native list geometry at {width}x{height}",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize(
    ("width", "height", "margin", "offset"),
    [(128, 128, 4, 1), (128, 160, 4, 1), (176, 220, 6, 2), (240, 320, 8, 3), (320, 240, 6, 2)],
)
def test_native_scrollbar_matches_list_inset_and_stays_visible_only_for_overflow(
    ui_lists: Path, width: int, height: int, margin: int, offset: int
) -> None:
    """The shared style keeps the same idle thumb after growing, scrolling and shrinking."""
    run_process(
        [str(ui_lists), "scrollbar", str(width), str(height), str(margin), str(offset)],
        name=f"native shared scrollbar appearance at {width}x{height}",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("presentation", ["icon", "detail", "inline", "virtual"])
def test_list_keypad_keeps_decorated_and_virtual_selection_visible(
    ui_lists: Path, presentation: str
) -> None:
    """Rows retain their text/detail/icon contents while native scrolling reveals focus."""
    run_process(
        [str(ui_lists), presentation, "128", "160", "4", "1"],
        name=f"native {presentation} list keypad selection",
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
