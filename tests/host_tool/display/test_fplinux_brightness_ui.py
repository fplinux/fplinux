# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of linked brightness controls and RGB565 render output."""

from __future__ import annotations

import json
import os
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import fplinux_ui_profile
import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.fixtures.psf_font import write_solid_ascii_font
from tests.host_tool.gui.png_capture import png_pixels
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

APP = ROOT / "alpine/aports/fplinux-brightness-ui"
SCREENS = ROOT / "site/scripts/screens"


def test_documentation_screen_uses_real_font_and_canonical_palette(tmp_path: Path) -> None:
    executable = tmp_path / "brightness-screen"
    frame = tmp_path / "brightness.rgb565"
    image = tmp_path / "brightness.png"
    font = Path("/opt/quality/fonts/ter-u16n-terminal.psf")
    assert font.is_file()
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            f"-I{APP}",
            f"-I{SCREENS}",
            str(SCREENS / "brightness-screen.c"),
            str(SCREENS / "screen-frame.c"),
            str(APP / "brightness-ui.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
            str(ROOT / "lib/fplinux/fplinux-font.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
        ],
        name="compile documentation brightness screen with the production palette",
    )
    run_process(
        [str(executable), "7", str(font), str(frame)],
        name="capture documentation brightness screen with Terminus",
        timeout=10,
        check=True,
    )
    run_process(
        ["python3", str(SCREENS / "rgb565-png.py"), "240", "320", str(frame), str(image)],
        name="encode documentation brightness capture",
        timeout=10,
        check=True,
    )
    width, height, pixels = png_pixels(image)
    assert (width, height) == (240, 320)
    colours = list(struct.iter_unpack("3B", pixels))
    assert set(colours) == {
        (16, 16, 16),
        (33, 32, 33),
        (222, 223, 222),
        (189, 199, 206),
        (173, 207, 222),
    }
    for first_row, last_row, text_colour in [
        (29, 61, (189, 199, 206)),
        (88, 168, (222, 223, 222)),
        (243, 259, (222, 223, 222)),
        (281, 297, (189, 199, 206)),
    ]:
        region = colours[first_row * width : last_row * width]
        assert text_colour in region
        assert (16, 16, 16) in region
    bar = colours[198 * width : 199 * width]
    assert bar.count((173, 207, 222)) == 119
    assert bar.count((33, 32, 33)) == 51
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        artifact = Path(log_root) / "brightness-screen"
        artifact.mkdir(parents=True, exist_ok=True)
        shutil.copy2(image, artifact / "brightness.png")


@pytest.fixture(scope="module")
def palette_profile(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "brightness-palette") as directory:
        tokens = json.loads((ROOT / "lib/fplinux/fplinux-ui-profile.json").read_text())
        tokens["themes"]["dark"] = json.loads(
            (ROOT / "tests/host_tool/gui/fixtures/raw-dark-palette.json").read_text()
        )
        source = directory / "profile.c"
        source.write_text(fplinux_ui_profile.render(tokens))
        yield source


@dataclass(frozen=True)
class BrightnessHarness:
    """Linked brightness program and its two immutable fixture fonts."""

    executable: Path
    small_font: Path
    large_font: Path


class BrightnessUiTests:
    """Exercise real keypad decisions and rendering at the supported phone sizes."""

    @pytest.fixture(scope="class")
    @classmethod
    def harness(
        cls,
        request: pytest.FixtureRequest,
        tmp_path_factory: pytest.TempPathFactory,
        palette_profile: Path,
    ) -> Iterator[BrightnessHarness]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "fplinux-brightness-ui") as temporary:
            executable = temporary / "brightness-ui-host"
            small_font = temporary / "small.psf"
            large_font = temporary / "large.psf"
            write_solid_ascii_font(small_font, width=6, height=12)
            write_solid_ascii_font(large_font, width=8, height=16)
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include/fplinux"),
                    "-I",
                    str(APP),
                    str(ROOT / "tests/host_tool/display/fplinux-brightness-ui.c"),
                    str(APP / "brightness-ui.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                    str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                    str(
                        palette_profile
                        if getattr(request, "param", None) == "variant"
                        else ROOT / "lib/fplinux/fplinux-ui-profile.c"
                    ),
                ],
                name="compile brightness UI host behavior",
                check=True,
            )
            yield BrightnessHarness(executable, small_font, large_font)

    def test_key_bounds_and_supported_display_sizes(self, harness: BrightnessHarness) -> None:
        """Keys clamp at 0/10; all five phone dimensions show ten bar segments."""
        result = run_process(
            [str(harness.executable), str(harness.small_font), str(harness.large_font)],
            name="run brightness UI host behavior",
            timeout=10,
            check=False,
        )
        assert (result.returncode) == (0), result.stderr

    @pytest.mark.parametrize("harness", ["variant"], indirect=True)
    @pytest.mark.parametrize(
        ("width", "height", "bar_x", "bar_y", "segment_width"),
        [
            (128, 128, 10, 79, 9),
            (128, 160, 10, 99, 9),
            (176, 220, 14, 136, 13),
            (240, 320, 17, 198, 17),
            (320, 240, 22, 148, 24),
        ],
    )
    @pytest.mark.parametrize(("level", "failed"), [(0, False), (5, False), (10, True)])
    def test_dark_palette_tokens_reach_text_and_scale_pixels(  # noqa: PLR0913 -- independent rendering inputs and literal bar geometry.
        self,
        harness: BrightnessHarness,
        tmp_path: Path,
        *,
        width: int,
        height: int,
        bar_x: int,
        bar_y: int,
        segment_width: int,
        level: int,
        failed: bool,
    ) -> None:
        """A changed shared palette reaches real text and scale pixels at each level."""
        frame = tmp_path / "frame"
        font = harness.small_font if width == 128 else harness.large_font
        result = run_process(
            [
                str(harness.executable),
                "--frame",
                str(width),
                str(height),
                str(font),
                str(level),
                "1" if failed else "0",
                str(frame),
            ],
            name=f"render generated brightness palette on {width}x{height}",
            timeout=10,
            check=False,
        )
        assert result.returncode == 0, result.stderr
        pixels = struct.unpack(f"<{width * height}H", frame.read_bytes())
        expected = {0x0000, 0x001F, 0x07E0}
        if failed:
            expected.add(0x0410)
        if level:
            expected.add(0x8010)
        if level < 10:
            expected.add(0x0800)
        assert set(pixels) == expected
        assert pixels[0] == 0x0000
        assert pixels[(height * 2 // 5) * width + width // 2] == 0x07E0
        font_width, font_height = (6, 12) if width == 128 else (8, 16)
        assert sum(pixel == 0x001F for pixel in pixels[: width * (height * 3 // 10)]) == (
            10 * font_width * font_height * 4
        )
        assert sum(pixel == 0x001F for pixel in pixels[width * (height * 88 // 100) :]) == (
            13 * font_width * font_height
        )
        message = pixels[width * (height * 70 // 100) : width * (height * 88 // 100)]
        # Solid fixture glyphs distinguish the normal and failed message ink.
        message_glyphs = 9 if failed else 8
        assert message.count(0x0410 if failed else 0x07E0) == (
            message_glyphs * font_width * font_height
        )
        bar = pixels[bar_y * width : (bar_y + 1) * width]
        assert bar.count(0x8010) == level * segment_width
        assert bar.count(0x0800) == (10 - level) * segment_width
        assert bar[:bar_x] == (0x0000,) * bar_x
        assert bar[bar_x] == (0x8010 if level else 0x0800)
        assert bar[-bar_x - 1] == (0x8010 if level == 10 else 0x0800)
        assert bar[-bar_x:] == (0x0000,) * bar_x
