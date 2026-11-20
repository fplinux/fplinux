# SPDX-License-Identifier: GPL-2.0-only
"""The production shell level panel with its pinned fonts and glyph masks."""

from __future__ import annotations

import importlib
import json
import os
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.display.status_resources import generate
from tests.host_tool.gui.support.rgb565_png import convert
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from types import ModuleType


# One row of 4-bit coverage per string for U+0021 in the pinned body fonts.
GALMURI_10_EXCLAMATION = ("f", "f", "f", "f", "f", "f", "0", "f", "f")
NOTO_13_EXCLAMATION = ("0e6", "0e5", "0d4", "0c4", "0b3", "0a2", "000", "0b4", "0d6")
NOTO_14_EXCLAMATION = ("d9", "c8", "b8", "b7", "a6", "95", "84", "00", "c8", "c8")


@dataclass(frozen=True)
class Frame:
    """The immutable panel pixels and screen placement returned by the renderer."""

    x: int
    y: int
    width: int
    height: int
    pixels: tuple[int, ...]


@pytest.fixture(scope="module")
def renderer(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path, ui_icon_resources: Path
) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "session-indicator-render") as directory:
        fonts = generate(directory, ui_host_inputs)
        executable = directory / "session-indicator-render"
        aport = ROOT / "alpine/aports/fplinux-session"
        compile_c(
            executable,
            [
                "-std=c11",
                "-pedantic",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{directory}",
                f"-I{ui_icon_resources.parent}",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{aport}",
                str(aport / "session-indicator-render.c"),
                str(ROOT / "lib/fplinux-shared/fplinux-status-screen.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
                str(fonts),
                str(ROOT / "tests/host_tool/gui/session/session-indicator-render-test.c"),
            ],
            name="compile the shell level renderer with production resources",
        )
        yield executable


def frame(  # noqa: PLR0913 -- independent profile, state, theme and visible chrome.
    renderer: Path,
    output: Path,
    width: int,
    height: int,
    *,
    theme: str,
    state: str,
    softkeys: bool = True,
) -> Frame:
    result = run_process(
        [
            str(renderer),
            str(width),
            str(height),
            theme,
            state,
            "softkeys" if softkeys else "fullscreen",
            str(output),
            "test",
        ],
        name=f"render shell {state} level on {width}x{height} {theme}",
        timeout=10,
        check=True,
    )
    geometry = json.loads(result.stdout)
    pixels = struct.unpack(f"={geometry['width'] * geometry['height']}H", output.read_bytes())
    return Frame(**geometry, pixels=pixels)


@pytest.mark.parametrize(
    ("width", "height", "margin", "softkey_height", "panel_height", "bar_y", "bar_height"),
    [
        (128, 128, 4, 16, 32, 24, 4),
        (128, 160, 4, 18, 32, 24, 4),
        (176, 220, 6, 24, 44, 32, 6),
        (240, 320, 8, 32, 56, 40, 8),
        (320, 240, 6, 22, 50, 36, 8),
    ],
)
@pytest.mark.parametrize(
    ("theme", "fill_colour", "track_colour"),
    [("dark", 0xAE7B, 0x3A29), ("light", 0x3B0E, 0xBE39)],
)
@pytest.mark.parametrize(
    ("state", "softkeys", "fill_fraction"),
    [
        ("ready", False, (7, 10)),
        ("ready", True, (7, 10)),
        ("zero", True, (0, 1)),
        ("full", True, (1, 1)),
        ("alarm", True, (3, 10)),
        ("brightness", True, (3, 4)),
    ],
)
def test_applied_level_scale_and_profile_placement(  # noqa: PLR0913 -- independent geometry, palette and chrome cases.
    renderer: Path,
    tmp_path: Path,
    *,
    width: int,
    height: int,
    margin: int,
    softkey_height: int,
    panel_height: int,
    bar_y: int,
    bar_height: int,
    theme: str,
    fill_colour: int,
    track_colour: int,
    state: str,
    softkeys: bool,
    fill_fraction: tuple[int, int],
) -> None:
    actual = frame(
        renderer, tmp_path / "panel", width, height, theme=theme, state=state, softkeys=softkeys
    )
    assert (actual.x, actual.y, actual.width, actual.height) == (
        margin,
        height - margin - panel_height - (softkey_height if softkeys else 0),
        width - 2 * margin,
        panel_height,
    )
    bar_width = actual.width - 2 * margin
    numerator, denominator = fill_fraction
    filled = bar_width * numerator // denominator
    for row in range(bar_y, bar_y + bar_height):
        assert actual.pixels[row * actual.width + margin : (row + 1) * actual.width - margin] == (
            (fill_colour,) * filled + (track_colour,) * (bar_width - filled)
        )
    assert actual.pixels[: actual.width] == (track_colour,) * actual.width
    assert actual.pixels[-actual.width :] == (track_colour,) * actual.width


@pytest.mark.parametrize(
    ("width", "height", "warning_right_offset", "warning_top", "alpha_rows"),
    [
        (128, 128, 35, 7, GALMURI_10_EXCLAMATION),
        (128, 160, 35, 7, GALMURI_10_EXCLAMATION),
        (176, 220, 42, 11, NOTO_13_EXCLAMATION),
        (240, 320, 48, 15, NOTO_14_EXCLAMATION),
        (320, 240, 44, 13, NOTO_14_EXCLAMATION),
    ],
)
@pytest.mark.parametrize(
    ("theme", "warning_colour", "background"),
    [("dark", 0xFDB5, 0x2945), ("light", 0xB0A3, 0xE73C)],
)
def test_pending_keeps_applied_frame_and_warning_adds_exclamation(  # noqa: PLR0913 -- independent profile and glyph expectations.
    renderer: Path,
    tmp_path: Path,
    *,
    width: int,
    height: int,
    theme: str,
    warning_colour: int,
    background: int,
    warning_right_offset: int,
    warning_top: int,
    alpha_rows: tuple[str, ...],
) -> None:
    ready = frame(renderer, tmp_path / "ready", width, height, theme=theme, state="ready")
    pending = frame(renderer, tmp_path / "pending", width, height, theme=theme, state="pending")
    warning = frame(renderer, tmp_path / "warning", width, height, theme=theme, state="warning")
    assert pending == ready
    assert (warning.x, warning.y, warning.width, warning.height) == (
        ready.x,
        ready.y,
        ready.width,
        ready.height,
    )
    changed_pixels = {
        offset: after
        for offset, (before, after) in enumerate(zip(ready.pixels, warning.pixels, strict=True))
        if before != after
    }
    warning_left = ready.width - warning_right_offset
    expected_pixels = {
        (warning_top + row) * ready.width + warning_left + col: blend_pixel(
            int(alpha, 16) * 17, warning_colour, background
        )
        for row, alpha_row in enumerate(alpha_rows)
        for col, alpha in enumerate(alpha_row)
        if alpha != "0"
    }
    assert changed_pixels == expected_pixels


@pytest.mark.parametrize(
    ("width", "height", "margin", "bar_y", "bar_height"),
    [
        (128, 128, 4, 24, 4),
        (128, 160, 4, 24, 4),
        (176, 220, 6, 32, 6),
        (240, 320, 8, 40, 8),
        (320, 240, 6, 36, 8),
    ],
)
@pytest.mark.parametrize(("theme", "background"), [("dark", 0x2945), ("light", 0xE73C)])
def test_unknown_and_unavailable_show_dashes_without_a_zero_scale(  # noqa: PLR0913 -- independent geometry and palette values.
    renderer: Path,
    tmp_path: Path,
    *,
    width: int,
    height: int,
    margin: int,
    bar_y: int,
    bar_height: int,
    theme: str,
    background: int,
) -> None:
    unknown = frame(renderer, tmp_path / "unknown", width, height, theme=theme, state="unknown")
    unavailable = frame(
        renderer, tmp_path / "unavailable", width, height, theme=theme, state="unavailable"
    )
    assert unknown == unavailable
    expected_row = (background,) * (unknown.width - 2 * margin)
    for row in range(bar_y, bar_y + bar_height):
        start = row * unknown.width + margin
        end = (row + 1) * unknown.width - margin
        assert unknown.pixels[start:end] == expected_row


@pytest.fixture
def png_reader(monkeypatch: pytest.MonkeyPatch) -> ModuleType:
    """Read the installed catalogue glyphs through their existing PNG reader."""
    monkeypatch.syspath_prepend(str(ROOT / "alpine/aports/fplinux-icons"))
    return importlib.import_module("svgraster")


def blend_pixel(alpha: int, foreground: int, background: int) -> int:
    """Blend an RGB565 pixel with alpha in the range 0..255."""
    channels = []
    for shift, mask in ((11, 31), (5, 63), (0, 31)):
        ink = (foreground >> shift) & mask
        paper = (background >> shift) & mask
        channels.append(((ink * alpha + paper * (255 - alpha) + 127) // 255) << shift)
    return channels[0] | channels[1] | channels[2]


@pytest.mark.parametrize(
    ("width", "height", "margin", "icon_size"),
    [
        (128, 128, 4, 16),
        (128, 160, 4, 16),
        (176, 220, 6, 20),
        (240, 320, 8, 24),
        (320, 240, 6, 24),
    ],
)
@pytest.mark.parametrize(
    ("theme", "foreground", "background"),
    [("dark", 0xDEFB, 0x2945), ("light", 0x18C3, 0xE73C)],
)
@pytest.mark.parametrize(
    ("state", "icon"),
    [("ready", "volume"), ("zero", "mute"), ("alarm", "alarm"), ("brightness", "brightness")],
)
def test_panel_uses_the_actual_catalogue_alpha_mask(  # noqa: PLR0913 -- selected resource and independent profile/palette inputs.
    renderer: Path,
    ui_icon_resources: Path,
    png_reader: ModuleType,
    tmp_path: Path,
    *,
    width: int,
    height: int,
    margin: int,
    icon_size: int,
    theme: str,
    foreground: int,
    background: int,
    state: str,
    icon: str,
) -> None:
    actual = frame(renderer, tmp_path / "panel", width, height, theme=theme, state=state)
    glyph = png_reader.decode_png(
        (ui_icon_resources / f"usr/share/fplinux/glyphs/{icon_size}/{icon}.png").read_bytes()
    )
    alpha = glyph.alpha()
    assert max(alpha) == 255
    for row in range(icon_size):
        expected = tuple(
            blend_pixel(value, foreground, background)
            for value in alpha[row * icon_size : (row + 1) * icon_size]
        )
        offset = (margin + row) * actual.width + margin
        assert actual.pixels[offset : offset + icon_size] == expected


@pytest.mark.parametrize(
    ("width", "height", "theme", "state", "chrome"),
    [
        (128, 128, "dark", "ready", "softkeys"),
        (176, 220, "dark", "warning", "softkeys"),
        (240, 320, "light", "zero", "fullscreen"),
        (320, 240, "light", "brightness", "softkeys"),
    ],
)
def test_retain_small_profile_theme_and_state_previews(  # noqa: PLR0913 -- four independently selected display scenes.
    renderer: Path, tmp_path: Path, *, width: int, height: int, theme: str, state: str, chrome: str
) -> None:
    output = tmp_path / "panel"
    run_process(
        [str(renderer), str(width), str(height), theme, state, chrome, str(output), "preview"],
        name=f"capture shell level indicator {width}x{height} {theme} {state}",
        timeout=10,
        check=True,
    )
    if log_root := os.environ.get("FPLINUX_LOG_ROOT"):
        Path(log_root).mkdir(parents=True, exist_ok=True)
        convert(
            width,
            height,
            Path(f"{output}-screen.rgb565"),
            Path(log_root) / f"system-level-{width}x{height}-{theme}-{state}-{chrome}.png",
        )
