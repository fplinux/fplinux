# SPDX-License-Identifier: GPL-2.0-only
"""The real bootstrap state adapter draws a static or failed frame."""

from __future__ import annotations

import json
import os
import struct
import sys
from itertools import groupby, pairwise
from typing import TYPE_CHECKING

import fplinux_ui_fonts
import fplinux_ui_profile
import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.display.status_resources import generate
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


type BootScreen = tuple[str, Path, fplinux_ui_fonts.ParsedFont]
type Ink = set[tuple[int, int]]


@pytest.fixture(scope="module", params=["128x128", "128x160", "176x220", "240x320", "320x240"])
def boot_screen(
    request: pytest.FixtureRequest, tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path
) -> Iterator[BootScreen]:
    profile = str(request.param)
    with c_build_directory(tmp_path_factory, f"boot-screen-{profile}") as directory:
        fonts = generate(directory, ui_host_inputs, profile)
        tokens = json.loads((ROOT / "lib/fplinux/fplinux-ui-profile.json").read_text())
        body = fplinux_ui_profile.font_slots(profile, tokens["profiles"][profile]["fonts"])[
            "font_body"
        ]
        body_asset = ui_host_inputs / "fonts" / fplinux_ui_fonts.font_spec(body).c_name
        body_font = fplinux_ui_fonts.parse_c_font(body_asset.read_text())
        generated = directory / "generated"
        generated.mkdir()
        (generated / "fplinux-status-profile.h").write_text(
            fplinux_ui_profile.render_status(tokens, profile)
        )
        run_process(
            [
                sys.executable,
                str(ROOT / "alpine/aports/fplinux-brand/generate-brand.py"),
                "--profile",
                str(ROOT / "lib/fplinux/fplinux-ui-profile.json"),
                "--boot-out",
                str(directory),
                "--screen",
                profile,
            ],
            name=f"generate bootstrap brand for {profile}",
            timeout=60,
            env=os.environ
            | {
                "PYTHONPATH": str(ROOT / "alpine/aports/fplinux-icons"),
                "NODE_PATH": "/opt/quality/node-tools/node_modules",
            },
            check=True,
        )
        executable = directory / "boot-screen"
        compile_c(
            executable,
            [
                "-std=c99",
                "-pedantic",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{directory}",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{ROOT / 'bootstrap/fplinux-boot-screen'}",
                str(ROOT / "tests/host_tool/display/fplinux-boot-screen.c"),
                str(ROOT / "bootstrap/fplinux-boot-screen/boot-screen.c"),
                str(ROOT / "lib/fplinux-shared/fplinux-status-screen.c"),
                str(fonts),
            ],
            name="compile selected bootstrap status screen",
        )
        yield profile, executable, body_font


def test_boot_and_failure_are_visible(boot_screen: BootScreen, tmp_path: Path) -> None:
    profile, executable, _ = boot_screen
    width, height = map(int, profile.split("x"))
    images = []
    for state in ("boot", "expected", "boot-offline", "expected-offline", "error", "error2"):
        output = tmp_path / state
        run_process(
            [str(executable), str(width), str(height), state, str(output)],
            name=f"render bootstrap {state} on {profile}",
            timeout=10,
            check=True,
        )
        images.append(struct.unpack(f"={width * height}H", output.read_bytes()))
    boot, expected, offline, expected_offline, error, other_error = images
    assert boot == expected
    assert offline == expected_offline
    assert offline != boot
    assert boot != error
    assert error != other_error
    assert 0xE71B in boot  # Light centre of the shared boot mark.
    mark_size = {"128x128": 48, "128x160": 64, "176x220": 80, "240x320": 96, "320x240": 96}[
        profile
    ]
    mark_top = {"128x128": 27, "128x160": 32, "176x220": 48, "240x320": 80, "320x240": 48}[profile]
    mark_pixels = [(i % width, i // width) for i, pixel in enumerate(boot) if pixel == 0xE71B]
    assert all(mark_top <= y < mark_top + mark_size for _, y in mark_pixels)
    assert any(pixel != 0x1082 for pixel in boot[width * (mark_top + mark_size) :])
    assert all(pixel == 0x1082 for pixel in boot[: width * (mark_top - 1)])
    assert 0xFDB5 in error  # Dark theme danger, without a full-width error bar.
    assert (
        max(error[y * width : (y + 1) * width].count(0xFDB5) for y in range(height)) < width // 2
    )


def ink_lines(pixels: tuple[int, ...], width: int) -> list[Ink]:
    background = pixels[0]
    rows = (
        {(x, y) for x, pixel in enumerate(pixels[start : start + width]) if pixel != background}
        for y, start in enumerate(range(0, len(pixels), width))
    )
    return [
        {point for row in band for point in row}
        for visible, band in groupby(rows, bool)
        if visible
    ]


def normalized_ink(ink: Ink) -> Ink:
    left = min(x for x, _ in ink)
    top = min(y for _, y in ink)
    return {(x - left, y - top) for x, y in ink}


def glyph_ink(font: fplinux_ui_fonts.ParsedFont, text: str) -> Ink:
    # Read the uncompressed LVGL font asset. Compose one line with no clipping or wrapping.
    ink: Ink = set()
    pen_x = 0
    for code, following in pairwise(map(ord, text + "\0")):
        glyph = font.glyphs[code]
        for index, alpha in enumerate(glyph.bitmap.hex()[: glyph.box_w * glyph.box_h]):
            if alpha != "0":
                row, column = divmod(index, glyph.box_w)
                ink.add((pen_x + glyph.ofs_x + column, row - glyph.box_h - glyph.ofs_y))
        adjustment = font.kerning.get((code, following), 0) * font.kern_scale // 16
        pen_x += (glyph.advance + adjustment + 8) // 16
    return normalized_ink(ink)


@pytest.mark.parametrize(
    "message",
    [
        "MICROSD NOT FOUND OR UNREADABLE. POWER OFF AND CHECK THE CARD",
        "FPLINUX.ITB NOT FOUND OR UNREADABLE. REWRITE THE BOOT CARD",
    ],
    ids=["card-error", "image-error"],
)
def test_boot_instructions_fit_and_remain_visible(
    boot_screen: BootScreen, tmp_path: Path, message: str
) -> None:
    profile, executable, body_font = boot_screen
    width, height = map(int, profile.split("x"))
    margin = {"128x128": 4, "128x160": 4, "176x220": 6, "240x320": 8, "320x240": 6}[profile]

    def render(detail: str) -> tuple[int, ...]:
        output = tmp_path / "frame"
        run_process(
            [
                str(executable),
                str(width),
                str(height),
                "error",
                str(output),
                detail,
            ],
            name=f"render error instruction on {profile}",
            timeout=10,
            check=True,
        )
        return struct.unpack(f"={width * height}H", output.read_bytes())

    heading_lines = len(ink_lines(render(""), width))
    instruction_lines = ink_lines(render(message), width)[heading_lines:]
    words = message.split()
    # A line can consume any whole-word prefix of the remaining instruction.
    for line in instruction_lines:
        assert all(margin <= x < width - margin and margin <= y < height - margin for x, y in line)
        observed = normalized_ink(line)
        for count in range(1, len(words) + 1):
            if observed == glyph_ink(body_font, " ".join(words[:count])):
                words = words[count:]
                break
        else:
            pytest.fail(
                f"{profile}: unexpected or clipped instruction line, expected {' '.join(words)!r}"
            )
    assert not words, f"{profile}: missing instruction text {' '.join(words)!r}"
