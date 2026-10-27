# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic and pinned TrueType faces consumed as LVGL bitmap font objects."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
from dataclasses import dataclass
from typing import TYPE_CHECKING

import fplinux_ui_fonts
import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.gui.font_support import synthetic_face
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture
def native_font_sources(tmp_path: Path) -> Path:
    """Let native abuild expose verified local originals through its source directory."""
    aport = tmp_path / "aport"
    aport.mkdir()
    data = synthetic_face()
    source = aport / "face.ttf"
    source.write_bytes(data)
    (aport / "fplinux-font-sources.json").write_text(
        json.dumps(
            {
                "converter_version": "1.5.3",
                "sources": {
                    "face.ttf": {
                        "url": "https://example.invalid/face.ttf",
                        "algorithm": "sha256",
                        "checksum": hashlib.sha256(data).hexdigest(),
                    }
                },
            }
        )
    )
    shutil.copyfile(
        ROOT / "tests/host_tool/gui/fixtures/font-sources/APKBUILD.fixture",
        aport / "APKBUILD",
    )
    run_process(
        ["abuild", "-F", "checksum", "fetch", "verify", "unpack"],
        name="expose verified font originals with native abuild",
        timeout=30,
        cwd=aport,
        env=os.environ | {"SRCDEST": str(tmp_path / "native-sources")},
        check=True,
    )
    return aport / "src"


def test_generated_bitmap_font_preserves_advance_cyrillic_and_kerning(
    tmp_path: Path, native_font_sources: Path, lvgl_library: tuple[Path, Path]
) -> None:
    assert (native_font_sources / "face.ttf").is_symlink()
    spec = fplinux_ui_fonts.FontSpec(
        "test-20",
        "face.ttf",
        20,
        digits=False,
        ranges=(("face.ttf", (0x20, *range(0x30, 0x3A), 0x41, 0x48, 0x56, 0x78, 0x410)),),
    )
    output = tmp_path / "output"
    fplinux_ui_fonts.build(
        [spec],
        native_font_sources,
        output,
        "/opt/quality/node-tools/node_modules/.bin/lv_font_conv",
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


def test_native_font_source_link_rejects_mismatched_original_bytes(
    native_font_sources: Path,
) -> None:
    """A supported native source link must still match the declared original checksum."""
    _, originals = fplinux_ui_fonts.source_manifest(
        native_font_sources / "fplinux-font-sources.json"
    )
    source = native_font_sources / "face.ttf"
    assert source.is_symlink()
    source.resolve().write_bytes(b"mismatched font bytes\n")
    with pytest.raises(fplinux_ui_fonts.FontBuildError, match=r"sha256 of .* pinned"):
        fplinux_ui_fonts.source_path(native_font_sources, source.name, originals)


@pytest.fixture(scope="module")
def pixel_font_consumer(
    tmp_path_factory: pytest.TempPathFactory,
    ui_host_inputs: Path,
    lvgl_library: tuple[Path, Path],
) -> Iterator[Path]:
    """Link the production-generated UI fonts to the real LVGL glyph reader."""
    lvgl_headers, lvgl_archive = lvgl_library
    with c_build_directory(tmp_path_factory, "pixel-font-consumer") as directory:
        output = directory / "font-consumer"
        compile_c(
            output,
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
                str(ROOT / "tests/host_tool/gui/fplinux-ui-pixel-fonts.c"),
                *(str(path) for path in sorted((ui_host_inputs / "fonts").glob("*.c"))),
                str(lvgl_archive),
                "-lm",
            ],
            name="link the real pixel UI fonts with the LVGL glyph reader",
        )
        yield output


@dataclass(frozen=True)
class PixelFontMetrics:
    """Native pixel metrics independently measured from the selected faces."""

    font_id: str
    line: int
    cap: int
    x_height: int
    digit_advance: int
    one_offset: int
    one_width: int
    h_width: int
    stem: int
    string_widths: tuple[int, int, int]


@pytest.mark.parametrize(
    "expected",
    [
        PixelFontMetrics("galmuri9-10", 14, 9, 6, 7, 1, 2, 5, 1, (14, 14, 70)),
        PixelFontMetrics("galmuri11-fauxbold-12", 17, 11, 8, 9, 2, 4, 8, 2, (18, 18, 90)),
    ],
    ids=["body", "title"],
)
def test_pixel_fonts_preserve_coverage_grid_metrics_and_kerning(
    pixel_font_consumer: Path,
    expected: PixelFontMetrics,
) -> None:
    result = run_process(
        [str(pixel_font_consumer), expected.font_id],
        name=f"read native {expected.font_id} pixel glyphs",
        timeout=10,
        check=True,
    )
    metrics = json.loads(result.stdout)
    assert metrics["own_glyphs"] == 323
    assert metrics["own_partial_alpha"] == 0
    assert metrics["line"] == expected.line
    assert metrics["cap"] == expected.cap
    assert metrics["x"] == expected.x_height
    assert metrics["digits"] == [expected.digit_advance] * 10
    assert metrics["digit_pairs"] == [expected.digit_advance] * 100
    assert metrics["strings"] == list(expected.string_widths)
    assert metrics["one_offset"] == expected.one_offset
    assert metrics["one_width"] == expected.one_width
    assert metrics["h_width"] == expected.h_width
    assert metrics["h_stem"] == expected.stem
    assert metrics["a_advance"] - metrics["av_advance"] == 1
    assert metrics["checkmark_pixels"] > 0
    assert metrics["checkmark_partial_alpha"] > 0
