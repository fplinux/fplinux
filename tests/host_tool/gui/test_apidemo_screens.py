# SPDX-License-Identifier: GPL-2.0-only
"""Real API Demos input, preferences and pixels through a native Wayland peer.

The peer controls session replies and reports unavailable media capabilities.
It does not emulate a motor, mixer or physical sound output.
"""

from __future__ import annotations

import importlib
import json
import os
import shutil
import struct
from collections import Counter
from dataclasses import dataclass
from itertools import pairwise
from pathlib import Path
from typing import TYPE_CHECKING, cast

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.host_tool.gui.list_rows import ListArea, frame_pixels, sequence_faults
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from types import ModuleType
    from typing import Any

SCREENS = ROOT / "site/scripts/screens"
CAPTURES = (
    "apidemo-no-results",
    "apidemo-catalogue",
    "apidemo-virtual-selection",
    "apidemo-choice",
    "apidemo-preferences",
    "apidemo-metrics",
    "apidemo-fullscreen",
    "apidemo-normal-view",
    "apidemo-frames",
    "apidemo-audio-unavailable",
    "apidemo-vibration-error",
)
PICKER_CAPTURES = (
    "apidemo-picker-textbox",
    "apidemo-picker-form-top",
    "apidemo-picker-form-scrolled",
    "apidemo-picker-canvas-first",
    "apidemo-picker-canvas-second",
)
POPUP_CAPTURES = (
    *PICKER_CAPTURES,
    "apidemo-text-options",
    "apidemo-image-link",
    "apidemo-image-button",
    "apidemo-links-options",
    "apidemo-long-options",
    "apidemo-long-options-end",
)
CANVAS_GEOMETRY = (
    ((128, 128), (128, 96), (0, 25, 128, 71), (60, 56, 8, 8)),
    ((128, 160), (128, 126), (0, 22, 128, 104), (60, 74, 8, 8)),
    ((176, 220), (176, 177), (0, 26, 176, 151), (88, 96, 10, 10)),
    ((240, 320), (240, 268), (0, 28, 240, 240), (120, 148, 12, 12)),
    ((320, 240), (320, 199), (0, 28, 320, 171), (154, 107, 12, 12)),
)


@pytest.fixture(scope="module")
def apidemo_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path, ui_icon_resources: Path
) -> Iterator[Path]:
    """Build the real app and peer against the shared native UI inputs."""
    with c_build_directory(tmp_path_factory, "apidemo-native") as directory:
        run_process(
            ["sh", str(SCREENS / "build-apidemo.sh"), str(directory), str(ui_host_inputs)],
            name="build actual API Demos and native capture peer",
            timeout=180,
            check=True,
        )
        (directory / "icons").symlink_to(ui_icon_resources, target_is_directory=True)
        yield directory


@pytest.fixture
def glyph_reader(monkeypatch: pytest.MonkeyPatch) -> ModuleType:
    """Read the actual generated resource pixels independently of the UI decoder."""
    monkeypatch.syspath_prepend(str(ROOT / "alpine/aports/fplinux-icons"))
    return importlib.import_module("svgraster")


def assert_popup_geometry(
    output: Path, picker: tuple[int, int, int, int], square_size: int
) -> None:
    for name in PICKER_CAPTURES:
        frame = json.loads((output / f"{name}.json").read_text(encoding="utf-8"))
        assert tuple(frame["picker"]) == picker
    for name in ("apidemo-image-link", "apidemo-image-button"):
        frame = json.loads((output / f"{name}.json").read_text(encoding="utf-8"))
        assert len(frame["images"]) == 2
        square, photo = frame["images"]
        for image in (square, photo):
            x, _, width, height = image["bounds"]
            frame_x, _, frame_width, frame_height = image["frame"]
            viewport_x, _, viewport_width, _ = image["viewport"]
            assert [width, height] == image["source"]
            assert frame_width > width > 0
            assert frame_height > height > 0
            assert viewport_x <= frame_x < x
            assert frame_x + frame_width <= viewport_x + viewport_width
        photo_width, photo_height = photo["source"]
        assert square["source"] == [square_size, square_size]
        assert 0 < photo_width <= 240
        assert 0 < photo_height <= 160
        assert photo_width > photo_height
        assert abs(2 * photo_width - 3 * photo_height) <= 3


def assert_menu_glyphs(  # noqa: PLR0913, PLR0917 -- capture, semantic expectations and resource reader are independent inputs.
    output: Path,
    name: str,
    expected: tuple[tuple[str, str], ...],
    native: Path,
    icon_size: int,
    reader: ModuleType,
) -> None:
    frame = json.loads((output / f"{name}.json").read_text(encoding="utf-8"))
    assert [row["text"] for row in frame["menu"]] == [text for text, _ in expected]
    for row, (_, glyph) in zip(frame["menu"], expected, strict=True):
        image = reader.decode_png(
            (native / f"icons/usr/share/fplinux/glyphs/{icon_size}/{glyph}.png").read_bytes()
        )
        assert row["icon"]
        assert bytes.fromhex(row["alpha"]) == image.alpha()


def run_canvas_scenario(  # noqa: PLR0913, PLR0917 -- each argument owns a native scenario input.
    native: Path,
    output: Path,
    screen: tuple[int, int],
    locale: str,
    theme: str,
    scenario: str,
) -> None:
    result = run_process(
        [
            str(native / "apidemo-screens"),
            *map(str, screen),
            str(native / "apidemo"),
            str(SCREENS / "apidemo-keyboard.xkb"),
            str(output / "session.sock"),
            str(output),
            locale,
            scenario,
            theme,
        ],
        name=f"actual API Demos {scenario} at {screen} ({theme}, {locale})",
        env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
        timeout=45,
    )
    (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            output,
            Path(log_root) / f"apidemo-{scenario}-{screen[0]}x{screen[1]}-{theme}-{locale}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr


def test_actual_russian_confirmation_title_scrolls_once_then_rests_with_dots(
    apidemo_native: Path, tmp_path: Path, rgb565_png: ModuleType
) -> None:
    """A one-line title moves out and back, then stays at rest in the native UI."""
    run_canvas_scenario(apidemo_native, tmp_path, (128, 160), "ru", "light", "confirmation-title")
    assert "Russian confirmation title scrolls once and rests with dots" in (
        tmp_path / "capture.log"
    ).read_text(encoding="utf-8")
    for name in ("scroll", "dots"):
        frame = tmp_path / f"apidemo-confirmation-title-{name}.rgb565"
        image = frame.with_suffix(".png")
        assert frame.stat().st_size == 128 * 126 * 2
        rgb565_png.convert(128, 126, frame, image)
        if log_root := os.environ.get("FPLINUX_LOG_ROOT"):
            shutil.copy2(image, Path(log_root) / "apidemo-confirmation-title-128x160-light-ru")


def test_actual_whole_surface_scrolls_lines_and_overlapping_pages(
    apidemo_native: Path, tmp_path: Path
) -> None:
    """The reported small-screen scene absorbs line tails and uses bounded pages."""
    run_canvas_scenario(apidemo_native, tmp_path, (128, 160), "ru", "light", "whole-scroll")
    log = (tmp_path / "capture.log").read_text(encoding="utf-8")
    observations = [
        tuple(map(int, line.split()[1:]))
        for line in log.splitlines()
        if line.startswith("whole-scroll ")
    ]
    assert [item[0] for item in observations] == [
        0,
        14,
        28,
        42,
        56,
        70,
        84,
        102,
        102,
        84,
        70,
        56,
        42,
        28,
        14,
        0,
        0,
        84,
        102,
        18,
        0,
    ]
    assert all(item[1:] == (108, 210, 14) for item in observations)


def test_actual_scrolling_text_pages_cover_long_content(
    apidemo_native: Path, tmp_path: Path
) -> None:
    """Both page directions cover a multi-page reader without skipping content."""
    run_canvas_scenario(apidemo_native, tmp_path, (128, 160), "en", "light", "reader-pages")
    log = (tmp_path / "capture.log").read_text(encoding="utf-8")
    observations = [
        tuple(map(int, line.split()[1:]))
        for line in log.splitlines()
        if line.startswith("reader-page ")
    ]
    assert len(observations) == 49
    _, height, content, line = observations[0]
    assert all(item[1:] == (height, content, line) for item in observations)
    last = content - height
    positions = [item[0] for item in observations]
    assert positions[0] == positions[-1] == 0
    assert positions[24] == last
    assert last > 2 * height
    assert positions[:25] == sorted(positions[:25])
    assert positions[24:] == sorted(positions[24:], reverse=True)
    for page in (positions[:25], positions[24:]):
        moves = [abs(right - left) for left, right in pairwise(page) if left != right]
        assert len(moves) > 2
        assert all(height - 2 * line < move <= height - line for move in moves[:-1])
        assert 0 < moves[-1] <= height - line


def pixel_contrast(first: int, second: int) -> float:
    def luminance(pixel: int) -> float:
        channels = ((pixel >> 11) / 31, ((pixel >> 5) & 63) / 63, (pixel & 31) / 31)
        linear = tuple(
            channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4
            for channel in channels
        )
        return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]

    light, dark = sorted((luminance(first), luminance(second)), reverse=True)
    return (light + 0.05) / (dark + 0.05)


@pytest.mark.parametrize(("screen", "surface", "canvas", "cursor"), CANVAS_GEOMETRY)
@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_sketch_cursor_frame_contrasts_with_empty_and_painted_cells(  # noqa: PLR0913, PLR0917 -- independent geometry and native inputs.
    apidemo_native: Path,
    tmp_path: Path,
    screen: tuple[int, int],
    surface: tuple[int, int],
    canvas: tuple[int, int, int, int],
    cursor: tuple[int, int, int, int],
    theme: str,
    locale: str,
) -> None:
    run_canvas_scenario(apidemo_native, tmp_path, screen, locale, theme, "sketch-cursor")
    width, height = surface
    left, top, cell, _ = cursor
    frames = {}
    for name in ("empty", "painted", "neighbour", "erased"):
        path = tmp_path / f"apidemo-sketch-{name}"
        geometry = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
        assert tuple(geometry["canvas"]) == canvas
        data = path.with_suffix(".rgb565").read_bytes()
        assert len(data) == width * height * 2
        frames[name] = tuple(value for (value,) in struct.iter_unpack("<H", data))
    borders = {}
    interiors = {}
    for name in ("empty", "painted"):
        pixels = frames[name]
        border = {
            pixels[y * width + x]
            for y in range(top, top + cell)
            for x in range(left, left + cell)
            if min(x - left, left + cell - 1 - x, y - top, top + cell - 1 - y) < 2
        }
        interior = {
            pixels[y * width + x]
            for y in range(top + 2, top + cell - 2)
            for x in range(left + 2, left + cell - 2)
        }
        assert len(border) == len(interior) == 1
        borders[name], interiors[name] = next(iter(border)), next(iter(interior))
        assert pixel_contrast(borders[name], interiors[name]) >= 4.5
    assert interiors["empty"] != interiors["painted"]
    assert borders["empty"] != borders["painted"]
    neighbour = frames["neighbour"]
    assert neighbour[(top + cell // 2) * width + left + cell // 2] == interiors["painted"]
    assert neighbour[top * width + left + cell] == borders["empty"]
    assert frames["erased"] == frames["empty"]


@pytest.mark.parametrize(("screen", "_surface", "canvas", "_cursor"), CANVAS_GEOMETRY)
@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_softkey_preview_draws_its_bar_only_in_full_screen_and_back_leaves_it(  # noqa: PLR0913, PLR0917 -- independent geometry and native inputs.
    apidemo_native: Path,
    tmp_path: Path,
    screen: tuple[int, int],
    _surface: tuple[int, int],
    canvas: tuple[int, int, int, int],
    _cursor: tuple[int, int, int, int],
    theme: str,
    locale: str,
) -> None:
    """The system bar alone shows the keys; in full screen the example's bar shows the same.

    The hint's last line stays reachable above whichever bar there is. Back
    in the integrated screen returns to the results; Back in full screen
    returns to the integrated screen.
    """
    run_canvas_scenario(apidemo_native, tmp_path, screen, locale, theme, "softkey-preview")
    russian = locale == "ru"
    hint_text = (
        "«Переключить полный экран» показывает вместо системной панели собственную "
        "панель примера с теми же клавишами. «Изменить подписи» меняет подпись OK, "  # noqa: RUF001 -- Russian text with Latin key names.
        "«Назад» сначала выходит из полного экрана, затем возвращает в каталог."
        if russian
        else "Toggle full screen shows this example's own legend bar in place of the system "
        "bar, with the same keys. Change legends updates OK's legend; Back leaves full "
        "screen, then returns."
    )
    full = (0, canvas[1], screen[0], screen[1] - canvas[1])

    def phase(name: str) -> dict[str, Any]:
        return cast(
            "dict[str, Any]",
            json.loads((tmp_path / f"apidemo-preview-{name}.json").read_text(encoding="utf-8")),
        )

    for name in ("top", "end", "changed", "normal", "back"):
        frame = phase(name)
        assert tuple(frame["canvas"]) == canvas, name
        objects = frame["canvas_objects"]
        hint = next(item for item in objects if item["text"] == hint_text)
        # No bar of its own: only the hint's viewport and the hint are shown.
        assert len(objects) == 2, (name, objects)
        assert tuple(objects[hint["parent"]]["bounds"]) == canvas, name
    end = phase("end")
    hint = next(item for item in end["canvas_objects"] if item["text"] == hint_text)
    viewport = end["canvas_objects"][hint["parent"]]["content"]
    assert (
        max(viewport[1], canvas[1]) <= hint["bounds"][1] + hint["bounds"][3] - hint["line_height"]
    )
    assert hint["bounds"][1] + hint["bounds"][3] <= min(
        viewport[1] + viewport[3], sum(canvas[1::2])
    )

    # Back in the integrated screen left it for the results.
    results = phase("results")
    assert all(item["text"] != hint_text for item in results["canvas_objects"])

    fullscreen = phase("fullscreen")
    assert tuple(fullscreen["canvas"]) == full
    objects = fullscreen["canvas_objects"]
    legends = [item for item in objects if item["parent"] >= 0 and item["text"] != hint_text]
    bar = objects[legends[0]["parent"]]
    _, bar_top, _, bar_height = bar["bounds"]
    assert bar_top + bar_height == full[1] + full[3]
    shown = [item["text"] for item in sorted(legends, key=lambda i: i["bounds"][0])]
    # The mode command, OK (changed once: it now restores) and Back.
    expected = (
        ("Переключить полный экран", "Вернуть подписи", "Назад")
        if russian
        else ("Toggle full screen", "Restore legends", "Back")
    )
    for text, whole in zip(shown, expected, strict=True):
        assert text == whole or (
            text.endswith("...") and len(text) > 3 and whole.startswith(text[:-3])
        ), (text, whole)
    hint = next(item for item in objects if item["text"] == hint_text)
    assert objects[hint["parent"]]["bounds"] == [full[0], full[1], full[2], full[3] - bar_height]


@pytest.mark.parametrize(("screen", "surface", "canvas", "_cursor"), CANVAS_GEOMETRY)
@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_canvas_bounds_contain_frames_fields_and_moving_shape(  # noqa: PLR0913, PLR0917 -- independent geometry and native inputs.
    apidemo_native: Path,
    tmp_path: Path,
    screen: tuple[int, int],
    surface: tuple[int, int],
    canvas: tuple[int, int, int, int],
    _cursor: tuple[int, int, int, int],
    theme: str,
    locale: str,
) -> None:
    run_canvas_scenario(apidemo_native, tmp_path, screen, locale, theme, "canvas-geometry")
    whole = json.loads((tmp_path / "apidemo-whole-geometry.json").read_text(encoding="utf-8"))
    assert whole["canvas"] == [0, 0, *surface]
    frame = next(item for item in whole["canvas_objects"] if item["border"] == 2)
    assert frame["bounds"] == whole["canvas"]
    padding, playground, cells, field_max = {
        (128, 128): (3, [4, 47, 106, 42], (6, 2), 63),
        (128, 160): (3, [4, 44, 106, 74], (6, 4), 96),
        (176, 220): (4, [6, 53, 152, 112], (7, 5), 139),
        (240, 320): (6, [8, 62, 208, 184], (8, 7), 224),
        (320, 240): (4, [6, 58, 300, 132], (12, 5), 159),
    }[screen]
    assert frame["padding"] == [padding] * 4
    for phase in ("first", "last"):
        capture = json.loads(
            (tmp_path / f"apidemo-moving-{phase}.json").read_text(encoding="utf-8")
        )
        assert tuple(capture["canvas"]) == canvas
        objects = capture["canvas_objects"]
        field_index = next(index for index, item in enumerate(objects) if item["border"] == 2)
        field = objects[field_index]
        assert field["bounds"] == playground
        assert field["padding"] == [padding] * 4
        square = next(item for item in objects if item["parent"] == field_index)
        x, y, width, height = field["bounds"]
        assert canvas[0] <= x < x + width <= canvas[0] + canvas[2]
        assert canvas[1] <= y < y + height <= canvas[1] + canvas[3]
        inner_x, inner_y, inner_width, inner_height = field["content"]
        square_x, square_y, square_width, square_height = square["bounds"]
        assert square_width == square_height
        assert inner_width % square_width == inner_height % square_height == 0
        assert (inner_width // square_width, inner_height // square_height) == cells
        expected = (
            (inner_x, inner_y)
            if phase == "first"
            else (
                inner_x + inner_width - square_width,
                inner_y + inner_height - square_height,
            )
        )
        assert (square_x, square_y) == expected
    fields = json.loads((tmp_path / "apidemo-fields-geometry.json").read_text(encoding="utf-8"))
    assert tuple(fields["canvas"]) == canvas
    column = next(item for item in fields["canvas_objects"] if item["parent"] == -1)
    assert tuple(column["bounds"]) == canvas
    bounded_fields = [item for item in fields["canvas_objects"] if item["max_height"] == field_max]
    assert len(bounded_fields) == 2
    assert all(item["bounds"][3] <= field_max for item in bounded_fields)


@pytest.mark.parametrize("arguments", [["-h"], ["--help"], ["--unknown", "--help"]])
def test_apidemo_help_precedes_preferences_and_ui(
    apidemo_native: Path, tmp_path: Path, arguments: list[str]
) -> None:
    state = tmp_path / "state"
    result = run_process(
        [str(apidemo_native / "apidemo"), *arguments],
        name="API Demos help before resources",
        env={
            "XDG_STATE_HOME": str(state),
            "FPLINUX_UI_SESSION_SOCKET": str(tmp_path / "missing.sock"),
            "WAYLAND_DISPLAY": "missing-apidemo-display",
        },
        timeout=10,
    )
    assert result.returncode == 0
    assert "Usage: fplinux-example-apidemo" in result.stdout
    assert result.stderr == ""
    assert not state.exists()


@pytest.mark.parametrize("argument", ["--unknown", "unexpected"])
def test_apidemo_invalid_arguments_precede_preferences_and_ui(
    apidemo_native: Path, tmp_path: Path, argument: str
) -> None:
    state = tmp_path / "state"
    result = run_process(
        [str(apidemo_native / "apidemo"), argument],
        name="API Demos invalid arguments before resources",
        env={
            "XDG_STATE_HOME": str(state),
            "FPLINUX_UI_SESSION_SOCKET": str(tmp_path / "missing.sock"),
            "WAYLAND_DISPLAY": "missing-apidemo-display",
        },
        timeout=10,
    )
    assert result.returncode == 2
    assert result.stdout == ""
    assert "fplinux-example-apidemo:" in result.stderr
    assert not state.exists()


def test_actual_apidemo_preferences_distinguish_memory_file_and_confirmed_save(
    apidemo_native: Path, tmp_path: Path
) -> None:
    result = run_process(
        [
            str(apidemo_native / "apidemo-screens"),
            "240",
            "320",
            str(apidemo_native / "apidemo"),
            str(SCREENS / "apidemo-keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            "en",
            "storage-failure",
        ],
        name="actual API Demos preferences under controlled write and sync failures",
        env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.endswith(
        "apidemo-storage: memory differs from file after ENOSPC; autosave has no modal spam; "
        "published sync failure remains unconfirmed; retry reopens 2\n"
    )
    preferences = tmp_path / "state/fplinux/org.fplinux.example.apidemo/preferences"
    assert "locale.count=2\n" in preferences.read_text(encoding="utf-8")


@pytest.mark.parametrize(
    ("screen", "surface", "locale"),
    [
        ((128, 128), (128, 96), "en"),
        ((128, 160), (128, 126), "en"),
        ((176, 220), (176, 177), "en"),
        ((240, 320), (240, 268), "en"),
        ((320, 240), (320, 199), "en"),
        ((240, 320), (240, 268), "ru"),
    ],
)
def test_actual_apidemo_search_choices_preferences_and_rendering(  # noqa: PLR0913, PLR0917 -- native inputs and diagnostic converter are independent resources.
    apidemo_native: Path,
    tmp_path_factory: pytest.TempPathFactory,
    rgb565_png: ModuleType,
    screen: tuple[int, int],
    surface: tuple[int, int],
    locale: str,
) -> None:
    output = tmp_path_factory.mktemp(f"apidemo-{screen[0]}-{screen[1]}-{locale}")
    try:
        result = run_process(
            [
                str(apidemo_native / "apidemo-screens"),
                *map(str, screen),
                str(apidemo_native / "apidemo"),
                str(SCREENS / "apidemo-keyboard.xkb"),
                str(output / "session.sock"),
                str(output),
                locale,
                "all",
            ],
            name=f"exercise actual API Demos at {screen[0]}x{screen[1]} ({locale})",
            env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
            timeout=60,
        )
        (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == 0, result.stdout + result.stderr
        captures: tuple[str, ...]
        if locale == "ru":
            assert result.stdout.endswith(
                "apidemo: Cyrillic uppercase search found translated list title\n"
            )
            captures = ("apidemo-russian-search",)
        else:
            for behavior in (
                (
                    "root and search remain available; category and result focus preserved; "
                    "query reused"
                ),
                "virtual row 2 selected",
                "real form choice changed to Large",
                "preferences reopened at 1; saved 2; hidden/show/close preserved state",
                "unavailable audio; asynchronous vibration error; close cleanup",
            ):
                assert behavior in result.stdout
            preferences = output / "state/fplinux/org.fplinux.example.apidemo/preferences"
            saved = dict(
                line.split("=", 1) for line in preferences.read_text(encoding="utf-8").splitlines()
            )
            assert saved["locale.name"] == ""
            assert saved["locale.count"] == "2"
            assert preferences.stat().st_mode & 0o777 == 0o600
            assert preferences.parent.stat().st_mode & 0o777 == 0o700
            captures = CAPTURES
        for name in captures:
            width, height = screen if name == "apidemo-fullscreen" else surface
            frame = output / f"{name}.rgb565"
            pixels = frame.read_bytes()
            assert len(pixels) == width * height * 2
            assert len(set(struct.iter_unpack("<H", pixels))) > 1
            assert f"{name}: {width}x{height}\n" in result.stdout
            rgb565_png.convert(width, height, frame, output / f"{name}.png")
    finally:
        log_root = os.environ.get("FPLINUX_LOG_ROOT")
        if log_root:
            shutil.copytree(
                output,
                Path(log_root) / f"apidemo-screens-{screen[0]}x{screen[1]}-{locale}",
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("session.sock"),
            )
        shutil.rmtree(output)


@pytest.mark.parametrize(
    ("screen", "surface", "picker", "icon_size", "square_size"),
    [
        ((128, 128), (128, 96), (1, 0, 126, 96), 16, 41),
        ((128, 160), (128, 126), (1, 15, 126, 96), 16, 48),
        ((176, 220), (176, 177), (5, 6, 166, 164), 20, 48),
        ((240, 320), (240, 268), (24, 44, 192, 180), 24, 48),
        ((320, 240), (320, 199), (20, 34, 280, 130), 24, 48),
    ],
)
@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_apidemo_catalogue_and_returning_search_profiles(  # noqa: PLR0913, PLR0917 -- explicit native inputs and expected surface.
    apidemo_native: Path,
    tmp_path: Path,
    screen: tuple[int, int],
    surface: tuple[int, int],
    picker: tuple[int, int, int, int],
    icon_size: int,
    square_size: int,
    theme: str,
    locale: str,
    glyph_reader: ModuleType,
    rgb565_png: ModuleType,
) -> None:
    """Use real app navigation, popup routes and generated action glyph pixels."""
    result = run_process(
        [
            str(apidemo_native / "apidemo-screens"),
            *map(str, screen),
            str(apidemo_native / "apidemo"),
            str(SCREENS / "apidemo-keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            locale,
            "navigation",
            theme,
        ],
        name=f"actual API Demos navigation at {screen} ({theme}, {locale})",
        env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
        timeout=90,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    try:
        assert result.returncode == 0, result.stdout + result.stderr
        assert (
            "real text, Form and both Canvas pickers; framed source images; "
            "Options icons and original callbacks" in result.stdout
        )
        assert_popup_geometry(tmp_path, picker, square_size)
        short_menu = (
            (("Показать", "view"), ("Вставить пример", "add"), ("Назад", "back"))
            if locale == "ru"
            else (("Show", "view"), ("Insert sample", "add"), ("Back", "back"))
        )
        long_menu = (
            (
                ("Сохранить", "save"),
                ("Отправить", "send"),
                ("Изменить", "edit"),
                ("Поиск", "search"),
                ("Добавить", "add"),
                ("Удалить", "delete"),
                ("Переименовать", "edit"),
                ("Справка", "help"),
            )
            if locale == "ru"
            else (
                ("Save", "save"),
                ("Send", "send"),
                ("Edit", "edit"),
                ("Search", "search"),
                ("Add", "add"),
                ("Delete", "delete"),
                ("Rename", "edit"),
                ("Help", "help"),
            )
        )
        links_menu = (
            (
                ("Показать фокус", "view"),
                ("Изменить кнопку", "edit"),
                ("Дать команду", "add"),
            )
            if locale == "ru"
            else (
                ("Show the focus", "view"),
                ("Change the button", "edit"),
                ("Give it a command", "add"),
            )
        )
        for name, expected in (
            ("apidemo-text-options", short_menu),
            ("apidemo-links-options", links_menu),
            ("apidemo-long-options", long_menu),
            ("apidemo-long-options-end", long_menu),
        ):
            assert_menu_glyphs(tmp_path, name, expected, apidemo_native, icon_size, glyph_reader)
        for name in (
            "apidemo-catalogue",
            "apidemo-search",
            "apidemo-search-results",
            *POPUP_CAPTURES,
        ):
            pixels = (tmp_path / f"{name}.rgb565").read_bytes()
            assert len(pixels) == surface[0] * surface[1] * 2
            assert len(set(struct.iter_unpack("<H", pixels))) > 1
            rgb565_png.convert(*surface, tmp_path / f"{name}.rgb565", tmp_path / f"{name}.png")
    finally:
        log_root = os.environ.get("FPLINUX_LOG_ROOT")
        if log_root:
            shutil.copytree(
                tmp_path,
                Path(log_root) / f"apidemo-navigation-{screen[0]}x{screen[1]}-{theme}-{locale}",
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("session.sock"),
            )


@dataclass(frozen=True)
class MenuCaptureGeometry:
    """Independent surface bounds and five-action menu heights."""

    screen: tuple[int, int]
    surface: tuple[int, int]
    menu_height: int
    title_height: int
    # Not all five rows fit below the title band: the menu's right edge is
    # its scroll channel.
    scrolls: bool = False


@pytest.mark.parametrize(
    "geometry",
    [
        # Fewer than three rows below the title band: three fill the surface.
        MenuCaptureGeometry((128, 128), (128, 96), 73, 25, scrolls=True),
        MenuCaptureGeometry((128, 160), (128, 126), 93, 22, scrolls=True),
        MenuCaptureGeometry((176, 220), (176, 177), 136, 26),
        MenuCaptureGeometry((240, 320), (240, 268), 166, 28),
        MenuCaptureGeometry((320, 240), (320, 199), 161, 28),
    ],
    ids=["128x128", "128x160", "176x220", "240x320", "320x240"],
)
@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_apidemo_compact_menu_pixels_preserve_background_and_legends(  # noqa: PLR0913, PLR0917 -- native inputs and diagnostic converter are independent resources.
    apidemo_native: Path,
    tmp_path: Path,
    rgb565_png: ModuleType,
    geometry: MenuCaptureGeometry,
    theme: str,
    locale: str,
) -> None:
    """The actual Canvas menu reaches the left edge and preserves its background."""
    result = run_process(
        [
            str(apidemo_native / "apidemo-screens"),
            *map(str, geometry.screen),
            str(apidemo_native / "apidemo"),
            str(SCREENS / "apidemo-keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            locale,
            "menu-layout",
            theme,
        ],
        name=f"actual compact Options at {geometry.screen} ({theme}, {locale})",
        env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
        timeout=30,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    width, height = geometry.surface
    try:
        assert result.returncode == 0, result.stdout + result.stderr
        frames = {}
        for phase in ("before", "open", "after"):
            frame = tmp_path / f"apidemo-menu-{phase}.rgb565"
            data = frame.read_bytes()
            assert len(data) == width * height * 2
            frames[phase] = [value for (value,) in struct.iter_unpack("<H", data)]
            rgb565_png.convert(width, height, frame, tmp_path / f"apidemo-menu-{phase}.png")
        opened = frames["open"]
        menu_top = height - geometry.menu_height
        line_start = menu_top * width
        line = opened[line_start : (menu_top + 1) * width]
        popup_colour = 0xE73C if theme == "light" else 0x2945
        channel_colour = 0xBE39 if theme == "light" else 0x3A29
        bottom = opened[(height - 1) * width : height * width]
        menu_width = next((index for index, colour in enumerate(line) if colour != 0xED4A), width)
        assert 0 < menu_width <= width
        assert (
            opened[line_start + width : line_start + width + menu_width] == [0xED4A] * menu_width
        )
        assert bottom[menu_width - 1] == (channel_colour if geometry.scrolls else popup_colour)
        static_pixels = min(menu_top, geometry.title_height) * width
        before = frames["before"][:static_pixels]
        covered = opened[:static_pixels]
        assert before == frames["after"][:static_pixels]
        assert before != covered
        background = Counter(before).most_common(1)[0][0]
        dimmed_background = Counter(covered).most_common(1)[0][0]
        assert all(
            actual == dimmed_background
            for original, actual in zip(before, covered, strict=True)
            if original == background
        )
        # The dimmed title band above the menu still shows its title.
        assert any(
            actual != dimmed_background
            for original, actual in zip(before, covered, strict=True)
            if original != background
        )
    finally:
        log_root = os.environ.get("FPLINUX_LOG_ROOT")
        if log_root:
            shutil.copytree(
                tmp_path,
                Path(log_root)
                / f"compact-menu-{geometry.screen[0]}x{geometry.screen[1]}-{theme}-{locale}",
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("session.sock"),
            )


@pytest.mark.parametrize(
    ("screen", "surface", "band", "pitches"),
    [
        ((128, 128), (128, 96), 25, (36, 24)),
        ((128, 160), (128, 126), 22, (35, 23)),
        ((176, 220), (176, 177), 26, (38, 27)),
        ((240, 320), (240, 268), 28, (43, 33)),
        ((320, 240), (320, 199), 28, (43, 32)),
    ],
    ids=["128x128", "128x160", "176x220", "240x320", "320x240"],
)
@pytest.mark.parametrize("theme", ["light", "dark"])
def test_actual_apidemo_lists_show_no_row_sliver_and_scroll_by_meaningful_steps(  # noqa: PLR0913, PLR0917 -- independent geometry and native inputs.
    apidemo_native: Path,
    tmp_path: Path,
    screen: tuple[int, int],
    surface: tuple[int, int],
    band: int,
    pitches: tuple[int, int],
    theme: str,
) -> None:
    """The detail-row catalogue and the virtual inbox keep rows whole or a real part."""
    result = run_process(
        [
            str(apidemo_native / "apidemo-screens"),
            *map(str, screen),
            str(apidemo_native / "apidemo"),
            str(SCREENS / "apidemo-keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            "en",
            "rows",
            theme,
        ],
        name=f"actual API demo list rows at {screen} ({theme})",
        env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
        timeout=60,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"apidemo-rows-{screen[0]}x{screen[1]}-{theme}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "apidemo-rows: catalogue and virtual inbox focus moved down and back" in result.stdout
    width, height = surface
    for (prefix, count), pitch in zip((("catalogue", 8), ("virtual", 12)), pitches, strict=True):
        area = ListArea(
            width=width,
            top=band,
            bottom=height,
            pitch=pitch,
            edge=(0, 2),
            divider=0xBE39 if theme == "light" else 0x3A29,
            highlight=0x3B0E if theme == "light" else 0xAE7B,
        )
        names = [f"apidemo-rows-{prefix}-down-{row:02}" for row in range(count)]
        names += [f"apidemo-rows-{prefix}-up-{row:02}" for row in range(count - 2, -1, -1)]
        frames = {
            name: frame_pixels((tmp_path / f"{name}.rgb565").read_bytes(), width) for name in names
        }
        assert sequence_faults(frames, area) == [], prefix


@pytest.mark.parametrize(
    ("scenario", "expected"),
    [
        ("duration", "Duration edits elapsed seconds and Reset restores 90 s"),
        ("whole-escape", "external Escape leaves Whole surface"),
        ("whole-phone-back", "phone Back leaves Whole surface"),
        ("canvas-clear-hold", "held Clear empties Canvas field; release stays; next Back leaves"),
    ],
)
def test_actual_apidemo_duration_and_canvas_keys(
    apidemo_native: Path,
    tmp_path_factory: pytest.TempPathFactory,
    scenario: str,
    expected: str,
) -> None:
    """Use the real Form editor and Canvas key routing through the app."""
    output = tmp_path_factory.mktemp(f"apidemo-{scenario}")
    try:
        result = run_process(
            [
                str(apidemo_native / "apidemo-screens"),
                "240",
                "320",
                str(apidemo_native / "apidemo"),
                str(SCREENS / "apidemo-keyboard.xkb"),
                str(output / "session.sock"),
                str(output),
                "en",
                scenario,
            ],
            name=f"exercise real API Demos {scenario}",
            env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
            timeout=30,
        )
        (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.endswith(f"apidemo: {expected}\n")
    finally:
        log_root = os.environ.get("FPLINUX_LOG_ROOT")
        if log_root:
            shutil.copytree(
                output,
                Path(log_root) / f"apidemo-{scenario}",
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("session.sock"),
            )
        shutil.rmtree(output)


@pytest.mark.parametrize("screen", [(128, 160), (240, 320), (320, 240)])
def test_open_releases_page_resources_and_preserves_confirmation_and_preferences_draft(
    apidemo_native: Path, tmp_path: Path, screen: tuple[int, int]
) -> None:
    result = run_process(
        [
            str(apidemo_native / "apidemo-screens"),
            *map(str, screen),
            str(apidemo_native / "apidemo"),
            str(SCREENS / "apidemo-keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            "en",
            "activation",
        ],
        name="actual API Demos activation releases page resources and preserves data",
        env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
        timeout=45,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.endswith(
        "apidemo-activation: same process; Resume kept watcher; Open released timerfd; "
        "confirmation and preference draft survived\n"
    )


def test_actual_apidemo_numeric_canvas_field_has_no_newline_command(
    apidemo_native: Path, tmp_path_factory: pytest.TempPathFactory
) -> None:
    output = tmp_path_factory.mktemp("apidemo-numeric")
    try:
        result = run_process(
            [
                str(apidemo_native / "apidemo-screens"),
                "240",
                "320",
                str(apidemo_native / "apidemo"),
                str(SCREENS / "apidemo-keyboard.xkb"),
                str(output / "session.sock"),
                str(output),
                "en",
                "numeric",
            ],
            name="exercise numeric API Demos Canvas field commands",
            env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
            timeout=15,
        )
        (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.endswith("apidemo: numeric Canvas field offers no New line command\n")
    finally:
        log_root = os.environ.get("FPLINUX_LOG_ROOT")
        if log_root:
            shutil.copytree(
                output,
                Path(log_root) / "apidemo-numeric",
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("session.sock"),
            )
        shutil.rmtree(output)
