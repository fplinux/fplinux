# SPDX-License-Identifier: GPL-2.0-only
"""Real converted UI fonts through the phoneME Canvas text core, without a VM."""

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
def phoneme_text_fonts(
    tmp_path_factory: pytest.TempPathFactory,
    ui_host_inputs: Path,
    lvgl_library: tuple[Path, Path],
) -> Iterator[Path]:
    """Reuse the production-generated font set and real LVGL glyph decoder."""
    headers, archive = lvgl_library
    canvas = ROOT / "alpine/aports/fplinux-phoneme/port/canvas"
    with c_build_directory(tmp_path_factory, "phoneme-text-fonts") as directory:
        executable = directory / "text-fonts"
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
                f"-I{canvas}",
                str(ROOT / "tests/host_tool/phoneme/text-fonts.c"),
                str(canvas / "fplinux-pm-glyph.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
                *(str(path) for path in sorted((ui_host_inputs / "fonts").glob("*.c"))),
                str(archive),
                "-lm",
            ],
            name="link phoneME Canvas text with the real converted UI fonts",
        )
        yield executable


@pytest.mark.parametrize("height", [128, 160])
def test_canvas_text_preserves_small_profile_metrics_and_english_russian_pixels(
    phoneme_text_fonts: Path, height: int
) -> None:
    result = run_process(
        [str(phoneme_text_fonts), str(height)],
        name=f"phoneME Canvas core metrics and pixels on 128x{height}",
        timeout=10,
        check=True,
    )
    assert result.stdout == (
        f"128x{height}: phoneME text preserves EN/RU metrics and pixel glyphs\n"
    )
