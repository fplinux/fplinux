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
