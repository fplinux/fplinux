# SPDX-License-Identifier: GPL-2.0-only
"""Exercise publication at the shared native guest frame observer."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_layout_reader_keeps_complete_frame_across_redraw(
    tmp_path: Path, lvgl_library: tuple[Path, Path]
) -> None:
    headers, archive = lvgl_library
    fixtures = ROOT / "tests/host_tool/guest_runtime/fixtures"
    ui = ROOT / "lib/fplinux"
    executable = tmp_path / "observe-redraw"
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
            f"-I{ROOT / 'include'}",
            f"-I{ROOT / 'include/fplinux'}",
            str(fixtures / "observe-redraw.c"),
            str(fixtures / "ui-observer.c"),
            str(ui / "fplinux-language.c"),
            str(ui / "fplinux-utf8.c"),
            str(archive),
            "-Wl,--wrap=fsync",
            "-Wl,--wrap=lv_display_flush_ready",
            "-Wl,--wrap=fplinux_language_load",
            "-Wl,--wrap=libintl_bindtextdomain",
            "-ljson-c",
            "-lintl",
            "-lm",
        ],
        name="link native LVGL redraws to the guest layout observer",
    )
    frames = tmp_path / "frames.log"
    layout = tmp_path / "frames.log.layout.json"
    result = run_process(
        [str(executable), str(layout)],
        name="read the previous observation across a new LVGL render",
        env={"FPLINUX_APP_FRAME_RECORD": str(frames)},
        timeout=10,
        check=True,
    )
    assert [label["text"] for label in json.loads(result.stdout)] == ["Before refresh"]
    assert [label["text"] for label in json.loads(layout.read_text())] == ["New snapshot"]
    assert frames.read_text() == "Before refresh|\nNew snapshot|\n"
