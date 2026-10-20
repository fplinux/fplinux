# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic TrueType faces converted into usable LVGL bitmap font objects."""

from __future__ import annotations

import hashlib
from typing import TYPE_CHECKING

import fplinux_ui_fonts

from tests import ROOT
from tests.c_harness import compile_c
from tests.host_tool.gui.font_support import synthetic_face
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

    import pytest


def test_generated_bitmap_font_preserves_advance_cyrillic_and_kerning(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, lvgl_library: tuple[Path, Path]
) -> None:
    data = synthetic_face()
    source = tmp_path / "face.ttf"
    source.write_bytes(data)
    # Replace only external font downloads with a controlled, checksum-pinned face.
    monkeypatch.setattr(
        fplinux_ui_fonts,
        "SOURCES",
        {
            "face.ttf": fplinux_ui_fonts.Source(
                "https://example.invalid/face.ttf", hashlib.sha256(data).hexdigest(), "test face"
            )
        },
    )
    spec = fplinux_ui_fonts.FontSpec(
        "test-20",
        "face.ttf",
        20,
        digits=False,
        ranges=(("face.ttf", (0x20, *range(0x30, 0x3A), 0x41, 0x48, 0x56, 0x78, 0x410)),),
    )
    output = tmp_path / "output"
    fplinux_ui_fonts.build(
        [spec], tmp_path, output, "/opt/quality/node-tools/node_modules/.bin/lv_font_conv"
    )
    executable = output / "font-consumer"
    lvgl_headers, lvgl_archive = lvgl_library
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-isystem",
            str(lvgl_headers),
            "-DLV_KCONFIG_IGNORE",
            "-DLV_LVGL_H_INCLUDE_SYSTEM",
            f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/fplinux-ui-fonts.c"),
            str(output / spec.c_name),
            str(output / "fplinux-ui-fonts.c"),
            str(lvgl_archive),
            "-lm",
        ],
        name="link generated bitmap font with the LVGL glyph reader",
    )
    result = run_process(
        [str(executable)], name="read generated bitmap glyphs", timeout=10, check=True
    )
    assert result.stdout == "latin=12 cyrillic=12 kerned=10 cap=14 x=10\n"
