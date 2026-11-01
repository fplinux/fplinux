# SPDX-License-Identifier: GPL-2.0-only
"""Pixel and bounds checks for the shared status-screen renderer."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.display.status_resources import generate
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def renderer(tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "status-screen") as directory:
        fonts = generate(directory, ui_host_inputs)
        executable = directory / "status-screen"
        compile_c(
            executable,
            [
                "-std=c11",
                "-pedantic",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{directory}",
                f"-I{ROOT / 'include/fplinux'}",
                str(ROOT / "tests/host_tool/display/fplinux-status-screen.c"),
                str(ROOT / "lib/fplinux/fplinux-status-screen.c"),
                str(ROOT / "lib/fplinux/fplinux-activity.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
                str(fonts),
            ],
            name="compile the shared status screen",
        )
        yield executable


def frame(  # noqa: PLR0913 -- independently selected screen, palette and state inputs.
    renderer: Path,
    output: Path,
    width: int,
    height: int,
    *,
    theme: str,
    state: str,
    phase: int = 0,
) -> tuple[int, ...]:
    run_process(
        [str(renderer), str(width), str(height), theme, state, str(phase), str(output)],
        name=f"render {state} on {width}x{height}",
        timeout=10,
        check=True,
    )
    return struct.unpack(f"={width * height}H", output.read_bytes())


@pytest.mark.parametrize(
    ("width", "height", "margin", "bar_y"),
    [
        (128, 128, 4, 54),
        (128, 160, 4, 70),
        (176, 220, 6, 99),
        (240, 320, 8, 143),
        (320, 240, 6, 105),
    ],
)
@pytest.mark.parametrize(
    ("theme", "filled", "track"), [("dark", 0xAE7B, 0x3A29), ("light", 0x3B0E, 0xBE39)]
)
@pytest.mark.parametrize(("state", "numerator"), [("zero", 0), ("middle", 1), ("full", 2)])
def test_real_progress_pixels_and_stride_bounds(  # noqa: PLR0913 -- geometry and palette are independent inputs.
    renderer: Path,
    tmp_path: Path,
    *,
    width: int,
    height: int,
    margin: int,
    bar_y: int,
    theme: str,
    filled: int,
    track: int,
    state: str,
    numerator: int,
) -> None:
    pixels = frame(renderer, tmp_path / "frame", width, height, theme=theme, state=state)
    bar_width = width - 2 * margin
    # UINT64_MAX / 2 is just below one half of the declared total.
    fill_width = (bar_width * numerator // 2) - (state == "middle")
    row = pixels[bar_y * width + margin : bar_y * width + width - margin]
    assert row == (filled,) * fill_width + (track,) * (bar_width - fill_width)


@pytest.mark.parametrize(
    ("width", "height"), [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)]
)
def test_busy_phases_and_terminal_states_have_distinct_pixels(
    renderer: Path, tmp_path: Path, width: int, height: int
) -> None:
    first = frame(renderer, tmp_path / "first", width, height, theme="dark", state="busy")
    second = frame(
        renderer, tmp_path / "second", width, height, theme="dark", state="busy", phase=1
    )
    error = frame(renderer, tmp_path / "error", width, height, theme="dark", state="error")
    ready = frame(renderer, tmp_path / "ready", width, height, theme="dark", state="ready")
    cancelled = frame(
        renderer, tmp_path / "cancelled", width, height, theme="dark", state="cancelled"
    )
    assert first != second
    assert error != first
    assert ready != error
    assert cancelled != ready


@pytest.mark.parametrize(
    ("width", "height", "bar_y"),
    [(128, 128, 54), (128, 160, 70), (176, 220, 99), (240, 320, 143), (320, 240, 105)],
)
def test_progress_values_do_not_move_the_heading_or_indicator(
    renderer: Path, tmp_path: Path, width: int, height: int, bar_y: int
) -> None:
    zero = frame(renderer, tmp_path / "zero", width, height, theme="dark", state="zero")
    full = frame(renderer, tmp_path / "full", width, height, theme="dark", state="full")
    assert zero[: bar_y * width] == full[: bar_y * width]
    assert zero[bar_y * width :] != full[bar_y * width :]


def test_glyph_baseline_kerning_and_alpha(tmp_path: Path) -> None:
    executable = tmp_path / "glyph"
    compile_c(
        executable,
        [
            "-std=c99",
            "-pedantic",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/display/fplinux-status-glyph.c"),
            str(ROOT / "lib/fplinux/fplinux-status-screen.c"),
        ],
        name="compile focused glyph rendering check",
    )
    run_process([str(executable)], name="check glyph pixels and baseline", timeout=10, check=True)
