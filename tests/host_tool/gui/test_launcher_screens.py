# SPDX-License-Identifier: GPL-2.0-only
"""Actual Launcher input, task messages and pixels through a native Wayland peer.

The peer owns the session and test icon tree; application and UI code stay real.
This host integration does not load or qualify a phone.
"""

from __future__ import annotations

import json
import os
import shutil
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, TypedDict, cast

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.host_tool.gui.frame_pixels import (
    assert_hands_pivot_on_centre,
    measure_dial,
    pixel,
    rgb565,
)
from tests.host_tool.gui.list_rows import ListArea, frame_pixels, sequence_faults
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SCREENS = ROOT / "site/scripts/screens"


class LabelObservation(TypedDict):
    """Visible native LVGL label text and physical rectangle."""

    text: str
    bounds: list[int]
    text_size: list[int]


class FrameObservation(TypedDict):
    """Widget metadata read after the corresponding actual frame was painted."""

    labels: list[LabelObservation]
    lines: int
    scroll_extent: int
    animations: int


def observed_frame(directory: Path, name: str) -> FrameObservation:
    """Read the completed frame's test-owned widget observation."""
    return cast(
        "FrameObservation", json.loads((directory / f"{name}.json").read_text(encoding="utf-8"))
    )


def retain_capture(directory: Path, name: str) -> None:
    """Keep native frames and observations with the public test run's logs."""
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            directory,
            Path(log_root) / name,
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )


@dataclass(frozen=True)
class LauncherGeometry:
    """Independent physical coordinates and page lengths for supported screens."""

    screen: tuple[int, int]
    page_rows: int
    row_pitch: int
    row_centre: int
    task_pitch: int
    task_centre: int
    icon_left: int
    icon_size: int
    # Each view's first row top and its tile's offset in the row cell; an
    # odd remainder of the cell lies below the tile.
    row_top: int
    row_tile: int
    task_top: int
    task_tile: int
    # The soft-key bar's line, which covers a row's bottom line ending there.
    bar_line: int


@pytest.fixture(scope="module")
def launcher_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path
) -> Iterator[Path]:
    """Link the actual app with the common runtime and a resource-tree boundary."""
    with c_build_directory(tmp_path_factory, "launcher-native") as directory:
        run_process(
            ["sh", str(SCREENS / "build-launcher.sh"), str(directory), str(ui_host_inputs)],
            name="build actual Launcher and native session peer",
            timeout=180,
            check=True,
        )
        yield directory


def red_tile_png(size: int) -> bytes:
    """Build a test-owned opaque red PNG using literal format fields."""

    def chunk(kind: bytes, data: bytes) -> bytes:
        checksum = zlib.crc32(kind + data)
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)

    rows = (b"\0" + b"\xff\0\0\xff" * size) * size
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


@pytest.fixture
def launcher_catalogue(tmp_path: Path) -> tuple[Path, Path]:
    """Own twelve installed entries and tiles for every supported tile size."""
    applications = tmp_path / "applications"
    icons = tmp_path / "icons"
    applications.mkdir()
    for index in range(1, 13):
        entry = applications / f"org.test.app{index:02}.desktop"
        entry.write_text(
            "[Desktop Entry]\nType=Application\n"
            f"Name=App {index:02}\nExec=/bin/true\nIcon=test-red\n"
            "Categories=Utility;\nX-FPLinux-Mode=integrated\n",
            encoding="utf-8",
        )
        entry.chmod(0o600)
    for size in (16, 20, 24, 32, 40, 48, 56, 60):
        directory = icons / f"{size}x{size}/apps"
        directory.mkdir(parents=True)
        tile = directory / "test-red.png"
        tile.write_bytes(red_tile_png(size))
        tile.chmod(0o600)
    return applications, icons


@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize(
    "geometry",
    [
        LauncherGeometry((128, 128), 3, 24, 52, 36, 58, 4, 16, 41, 3, 41, 9, 112),
        LauncherGeometry((128, 160), 4, 23, 49, 35, 55, 4, 16, 38, 3, 38, 9, 142),
        LauncherGeometry((176, 220), 5, 27, 58, 38, 63, 6, 20, 45, 3, 45, 8, 196),
        LauncherGeometry((240, 320), 7, 33, 64, 43, 69, 8, 24, 48, 4, 48, 9, 288),
        LauncherGeometry((320, 240), 5, 32, 62, 43, 68, 6, 24, 47, 3, 47, 9, 218),
    ],
    ids=["128x128", "128x160", "176x220", "240x320", "320x240"],
)
def test_actual_launcher_list_navigation_tasks_and_launch_pixels(
    launcher_native: Path,
    launcher_catalogue: tuple[Path, Path],
    tmp_path: Path,
    geometry: LauncherGeometry,
    theme: str,
) -> None:
    """Observe real selection, overlays and tile rings in both catalogue and Tasks."""
    applications, icons = launcher_catalogue
    width, height = geometry.screen
    result = run_process(
        [
            str(launcher_native / "launcher-screens"),
            str(width),
            str(height),
            str(launcher_native / "launcher"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(applications),
            str(icons),
            theme,
            str(geometry.page_rows),
        ],
        name=f"actual Launcher List and Tasks at {width}x{height} ({theme})",
        timeout=60,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"launcher-screens-{width}x{height}-{theme}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert (
        "launcher-tasks: Home opens Tasks; Cancel keeps app; Close and Open target exact instances"
        in result.stdout
    )
    accent, background, ring, divider = (
        (0x3B0E, 0xF7BE, 0xFFFF, 0xBE39) if theme == "light" else (0xAE7B, 0x1082, 0x09A8, 0x3A29)
    )
    for view, pitch, centre, first, tile in (
        ("list", geometry.row_pitch, geometry.row_centre, geometry.row_top, geometry.row_tile),
        (
            "tasks",
            geometry.task_pitch,
            geometry.task_centre,
            geometry.task_top,
            geometry.task_tile,
        ),
    ):
        frames = [
            (tmp_path / f"launcher-{view}-{selection}.rgb565").read_bytes()
            for selection in ("first", "second")
        ]
        for selected, frame in enumerate(frames):
            assert len(frame) == width * height * 2
            for row in (0, 1):
                y = centre + row * pitch
                assert (
                    pixel(frame, width, geometry.icon_left + geometry.icon_size // 2, y) == 0xF800
                )
                assert pixel(frame, width, geometry.icon_left - 1, y) == (
                    ring if row == selected else background
                )
                assert pixel(frame, width, width - 12, y) == (
                    accent if row == selected else background
                )
                # Down the tile's middle: the cell, the selection ring, the
                # tile, the same space below it and the row's divider; the
                # second of the two tasks is the last row, without one, unless
                # it ends on the soft-key bar's line.
                top = first + row * pitch
                below = pitch - 1 - geometry.icon_size - tile
                fill = accent if row == selected else background
                edge = ring if row == selected else fill
                last_task = view == "tasks" and row == 1
                divider_line = (
                    background if last_task and top + pitch - 1 != geometry.bar_line else divider
                )
                column = [
                    pixel(frame, width, geometry.icon_left + geometry.icon_size // 2, y)
                    for y in range(top, top + pitch)
                ]
                assert column == [
                    *[fill] * (tile - 1),
                    edge,
                    *[0xF800] * geometry.icon_size,
                    edge,
                    *[fill] * (below - 1),
                    divider_line,
                ], f"{view} row {row} with row {selected} selected"
    restored = (tmp_path / "launcher-returned-list.rgb565").read_bytes()
    assert pixel(restored, width, geometry.icon_left - 1, geometry.row_centre) == background
    assert (
        pixel(restored, width, geometry.icon_left - 1, geometry.row_centre + geometry.row_pitch)
        == ring
    )
    launched = (tmp_path / "launcher-launch.rgb565").read_bytes()
    assert len(launched) == width * height * 2
    assert pixel(launched, width, width - 12, geometry.row_centre) == background
    assert b"\x00\xf8" in launched
    notice = (tmp_path / "launcher-notice.rgb565").read_bytes()
    closed = (tmp_path / "launcher-notice-closed.rgb565").read_bytes()
    assert len(notice) == len(closed) == width * height * 2
    assert notice != closed


@pytest.mark.parametrize(
    ("screen", "bars", "pitch"),
    [
        ((128, 128), (41, 16), 24),
        ((128, 160), (38, 18), 23),
        ((176, 220), (45, 24), 27),
        ((240, 320), (48, 32), 33),
        ((320, 240), (47, 22), 32),
    ],
    ids=["128x128", "128x160", "176x220", "240x320", "320x240"],
)
@pytest.mark.parametrize("theme", ["light", "dark"])
def test_actual_launcher_list_shows_no_row_sliver_and_scrolls_by_meaningful_steps(  # noqa: PLR0913, PLR0917 -- independent geometry and native inputs.
    launcher_native: Path,
    launcher_catalogue: tuple[Path, Path],
    tmp_path: Path,
    screen: tuple[int, int],
    bars: tuple[int, int],
    pitch: int,
    theme: str,
) -> None:
    """Moving List focus down twelve apps and back keeps rows whole or a real part."""
    applications, icons = launcher_catalogue
    width, height = screen
    result = run_process(
        [
            str(launcher_native / "launcher-screens"),
            str(width),
            str(height),
            str(launcher_native / "launcher"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(applications),
            str(icons),
            theme,
            "1",
            "rows",
            "en",
        ],
        name=f"actual Launcher List rows at {width}x{height} ({theme})",
        timeout=60,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    retain_capture(tmp_path, f"launcher-rows-{width}x{height}-{theme}")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "launcher-rows: List focus moved down to app 12 and back to app 01" in result.stdout
    # The list area lies between the title band and the soft-key bar; its
    # rows leave the left edge columns free.
    area = ListArea(
        width=width,
        top=bars[0],
        bottom=height - bars[1],
        pitch=pitch,
        edge=(0, 2),
        divider=0xBE39 if theme == "light" else 0x3A29,
        highlight=0x3B0E if theme == "light" else 0xAE7B,
    )
    names = [f"launcher-rows-down-{index:02}" for index in range(1, 13)]
    names += [f"launcher-rows-up-{index:02}" for index in range(11, 0, -1)]
    frames = {
        name: frame_pixels((tmp_path / f"{name}.rgb565").read_bytes(), width) for name in names
    }
    assert sequence_faults(frames, area) == []
    assert frames[names[0]] == frames[names[-1]]


def install_clock_entry(applications: Path) -> None:
    """Declare the ordinary installed Clock consumed by Home's shortcut."""
    entry = applications / "org.fplinux.clock.desktop"
    entry.write_text(
        "[Desktop Entry]\nType=Application\nName=Clock\nName[ru]=Часы\n"
        "Exec=/bin/true\nIcon=test-red\nCategories=Utility;\nX-FPLinux-Mode=integrated\n",
        encoding="utf-8",
    )
    entry.chmod(0o600)


def capture_standby(  # noqa: PLR0913 -- independent native boundary inputs.
    launcher_native: Path,
    catalogue: tuple[Path, Path],
    directory: Path,
    screen: tuple[int, int],
    *,
    theme: str,
    locale: str,
    clock_installed: bool,
) -> None:
    applications, icons = catalogue
    assert (applications / "org.fplinux.clock.desktop").is_file() == clock_installed
    result = run_process(
        [
            str(launcher_native / "launcher-screens"),
            *map(str, screen),
            str(launcher_native / "launcher"),
            str(SCREENS / "keypad.xkb"),
            str(directory / "session.sock"),
            str(directory),
            str(applications),
            str(icons),
            theme,
            "1",
            "home",
            locale,
            "installed" if clock_installed else "absent",
        ],
        name=f"actual Launcher standby at {screen} in {theme}/{locale}",
        env={
            "FPLINUX_UI_LOCALE_DIRECTORY": str(launcher_native / "locale"),
            "FPLINUX_LAUNCHER_LANGUAGE_DIRECTORY": str(ROOT / "alpine/aports/fplinux-language"),
        },
        timeout=30,
    )
    (directory / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    clock = "installed" if clock_installed else "absent"
    retain_capture(directory, f"launcher-home-{screen[0]}x{screen[1]}-{theme}-{locale}-{clock}")
    assert result.returncode == 0, result.stdout + result.stderr
    assert (
        "launcher-home: simultaneous events; local countdown; hidden timer paused" in result.stdout
    )


@pytest.mark.parametrize("screen", [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)])
@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_home_shows_simultaneous_events_and_selected_clock(  # noqa: PLR0913 -- explicit matrix inputs.
    launcher_native: Path,
    launcher_catalogue: tuple[Path, Path],
    tmp_path: Path,
    screen: tuple[int, int],
    *,
    theme: str,
    locale: str,
) -> None:
    """Home keeps all events, ticks local deadlines and launches the installed Clock."""
    install_clock_entry(launcher_catalogue[0])
    capture_standby(
        launcher_native,
        launcher_catalogue,
        tmp_path,
        screen,
        theme=theme,
        locale=locale,
        clock_installed=True,
    )
    idle = observed_frame(tmp_path, "launcher-home-idle")
    events = observed_frame(tmp_path, "launcher-home-events")
    analog = observed_frame(tmp_path, "launcher-home-analog")
    idle_text = [label["text"] for label in idle["labels"]]
    event_text = [label["text"] for label in events["labels"]]
    date_fragment = "октября" if locale == "ru" else "October"
    assert "07:00" in idle_text
    assert any(date_fragment in text for text in idle_text)
    assert ("Часы" if locale == "ru" else "Clock") in idle_text
    alarm_prefix = "Будильник 07:30" if locale == "ru" else "Alarm 07:30"
    alarm = next(text for text in event_text if text.startswith(alarm_prefix))
    if screen[0] >= 176:
        assert alarm.endswith(" · Wake")
    assert ("Пауза: 01:30" if locale == "ru" else "Paused: 01:30") in event_text
    assert ("Пропущено: 3" if locale == "ru" else "Missed: 3") in event_text
    assert any(date_fragment in text for text in event_text) == (screen != (128, 128))
    assert idle["lines"] == 0
    assert analog["lines"] == 3
    assert (tmp_path / "launcher-home-events.rgb565").read_bytes() != (
        tmp_path / "launcher-home-analog.rgb565"
    ).read_bytes()
    body_top, body_bottom = {
        (128, 128): (16, 112),
        (128, 160): (16, 142),
        (176, 220): (19, 196),
        (240, 320): (20, 288),
        (320, 240): (19, 218),
    }[screen]
    for frame in (events, analog):
        event_rows = [
            label
            for label in frame["labels"]
            if label["text"].startswith(
                ("Alarm ", "Будильник ", "Paused:", "Пауза:", "Missed:", "Пропущено:")
            )
        ]
        assert len(event_rows) == 3
        previous_bottom = body_top
        for label in sorted(event_rows, key=lambda item: item["bounds"][1]):
            x, y, width, height = label["bounds"]
            assert x >= 4
            assert x + width <= screen[0] - 4
            assert y >= previous_bottom
            assert y + height <= body_bottom
            previous_bottom = y + height
            if not label["text"].startswith(("Alarm ", "Будильник ")):
                assert label["text_size"][0] <= width
                assert label["text_size"][1] <= height


# Body rows between the bars and the stack's spacing on each screen.
HOME_BODY = {
    (128, 128): (16, 112, 4),
    (128, 160): (16, 142, 4),
    (176, 220): (19, 196, 6),
    (240, 320): (20, 288, 8),
    (320, 240): (19, 218, 6),
}
HOME_COLOURS = {
    "light": {"background": "#F7F7F7", "ink": "#181818", "second": "#A55D00"},
    "dark": {"background": "#101010", "ink": "#DEDFDE", "second": "#EFAA52"},
}


@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("screen", list(HOME_BODY), ids=lambda screen: "x".join(map(str, screen)))
def test_actual_home_clocks_use_brand_for_colon_and_second_hand(
    launcher_native: Path,
    launcher_catalogue: tuple[Path, Path],
    tmp_path: Path,
    screen: tuple[int, int],
    theme: str,
) -> None:
    """The 07:00 colon and the 07:00:00 second hand carry the brand colour."""
    install_clock_entry(launcher_catalogue[0])
    capture_standby(
        launcher_native,
        launcher_catalogue,
        tmp_path,
        screen,
        theme=theme,
        locale="en",
        clock_installed=True,
    )
    width, _ = screen
    body_top, body_bottom, gap = HOME_BODY[screen]
    colours = {name: rgb565(colour) for name, colour in HOME_COLOURS[theme].items()}
    frame = (tmp_path / "launcher-home-analog.rgb565").read_bytes()
    dial = measure_dial(frame, width, body_top, colours["background"], colours["second"])
    last_ink = max(
        y
        for y in range(body_top, body_bottom)
        if any(pixel(frame, width, x, y) != colours["background"] for x in range(width))
    )
    assert dial.top - body_top >= gap
    assert body_bottom - 1 - last_ink >= gap
    assert_hands_pivot_on_centre(frame, width, dial, colours["ink"])
    brand = [
        (x, y)
        for y in range(body_top, body_bottom)
        for x in range(width)
        if pixel(frame, width, x, y) == colours["second"]
    ]
    assert brand
    assert all(dial.top <= y <= dial.bottom for _, y in brand)

    digital = (tmp_path / "launcher-home-idle.rgb565").read_bytes()
    label = next(
        item
        for item in observed_frame(tmp_path, "launcher-home-idle")["labels"]
        if item["text"] == "07:00"
    )
    _, top, _, height = label["bounds"]
    brand = [
        (x, y)
        for y in range(body_top, body_bottom)
        for x in range(width)
        if pixel(digital, width, x, y) == colours["second"]
    ]
    assert brand
    assert all(top <= y < top + height for _, y in brand)
    assert max(abs(x - width // 2) for x, _ in brand) <= 3


def test_home_without_installed_clock_has_no_dead_shortcut(
    launcher_native: Path, launcher_catalogue: tuple[Path, Path], tmp_path: Path
) -> None:
    """The actual Home's empty right slot sends no launch on its physical softkey."""
    capture_standby(
        launcher_native,
        launcher_catalogue,
        tmp_path,
        (128, 128),
        theme="light",
        locale="en",
        clock_installed=False,
    )
    labels = observed_frame(tmp_path, "launcher-home-idle")["labels"]
    assert "Clock" not in [label["text"] for label in labels]


@pytest.mark.parametrize("count", [5, 6, 7, 8, 12])
@pytest.mark.parametrize("theme", ["light", "dark"])
def test_landscape_grid_fits_two_rows_without_false_scrollbar(
    launcher_native: Path,
    launcher_catalogue: tuple[Path, Path],
    tmp_path: Path,
    count: int,
    theme: str,
) -> None:
    """Actual five-to-eight-app grids balance margins; larger catalogues retain scroll."""
    applications, icons = launcher_catalogue
    for index in range(count + 1, 13):
        (applications / f"org.test.app{index:02}.desktop").unlink()
    result = run_process(
        [
            str(launcher_native / "launcher-screens"),
            "320",
            "240",
            str(launcher_native / "launcher"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(applications),
            str(icons),
            theme,
            "1",
            "grid",
            "en",
        ],
        name=f"actual landscape Launcher with {count} installed entries",
        timeout=30,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    retain_capture(tmp_path, f"launcher-grid-320x240-{count}-{theme}")
    assert result.returncode == 0, result.stdout + result.stderr
    observed = observed_frame(tmp_path, "launcher-grid-fit")
    assert (observed["scroll_extent"] == 0) == (count <= 8)
    # The band's 22 px title line lies in the 27 rows above its divider,
    # 2 above and 3 below, under the 19 px status bar.
    title = next(label for label in observed["labels"] if label["text"] == "App 01")
    assert title["bounds"][1::2] == [21, 22]
    if count <= 8:
        frame = (tmp_path / "launcher-grid-fit.rgb565").read_bytes()
        assert pixel(frame, 320, 46, 67) == 0xF800
        assert pixel(frame, 320, 46, 143) == 0xF800
        assert pixel(frame, 320, 274, 67) == 0xF800
        assert pixel(frame, 320, 16, 64) == 0xF800
        assert pixel(frame, 320, 303, 64) == 0xF800
        red_rows = [y for y in range(46, 216) if pixel(frame, 320, 46, y) == 0xF800]
        assert red_rows == [*range(64, 124), *range(141, 201)]


Rect = tuple[int, int, int, int]


@dataclass(frozen=True)
class BatteryPixels:
    """Literal battery icon pixels of one screen, as x, y, width, height."""

    body: Rect
    cap: Rect
    bar_size: tuple[int, int]
    bar_x: tuple[int, ...]
    low_stem: Rect
    low_dot: Rect
    # The bolt's top-left pixel and its rows, "#" for ink.
    bolt_origin: tuple[int, int]
    bolt: tuple[str, ...]


BOLT_7 = ("...#", "..#.", ".#..", "####", "..#.", ".#..", "#...")
BOLT_9 = ("....#", "...#.", "..#..", ".#...", "#####", "...#.", "..#..", ".#...", "#....")
BOLT_11 = (
    ".....#",
    "....#.",
    "...#..",
    "..#...",
    ".#....",
    "######",
    "....#.",
    "...#..",
    "..#...",
    ".#....",
    "#.....",
)
NARROW_BATTERY = BatteryPixels(
    body=(105, 3, 18, 8),
    cap=(123, 5, 1, 4),
    bar_size=(2, 4),
    bar_x=(107, 110, 113, 116, 119),
    low_stem=(113, 5, 2, 2),
    low_dot=(113, 8, 2, 1),
    bolt_origin=(99, 4),
    bolt=BOLT_7,
)
BATTERY_PIXELS = {
    (128, 128): NARROW_BATTERY,
    (128, 160): NARROW_BATTERY,
    (176, 220): BatteryPixels(
        body=(145, 4, 23, 10),
        cap=(168, 7, 2, 4),
        bar_size=(3, 6),
        bar_x=(147, 151, 155, 159, 163),
        low_stem=(155, 6, 3, 4),
        low_dot=(155, 11, 3, 1),
        bolt_origin=(137, 4),
        bolt=BOLT_9,
    ),
    (240, 320): BatteryPixels(
        body=(207, 3, 23, 12),
        cap=(230, 6, 2, 6),
        bar_size=(3, 8),
        bar_x=(209, 213, 217, 221, 225),
        low_stem=(217, 5, 3, 5),
        low_dot=(217, 11, 3, 2),
        bolt_origin=(197, 4),
        bolt=BOLT_11,
    ),
    (320, 240): BatteryPixels(
        body=(289, 3, 23, 12),
        cap=(312, 6, 2, 6),
        bar_size=(3, 8),
        bar_x=(291, 295, 299, 303, 307),
        low_stem=(299, 5, 3, 5),
        low_dot=(299, 11, 3, 2),
        bolt_origin=(280, 3),
        bolt=BOLT_11,
    ),
}
# Status-bar heights without the divider row below them.
STATUS_ROWS = {(128, 128): 15, (128, 160): 15, (176, 220): 18, (240, 320): 19, (320, 240): 18}
# Shown battery level, low "!" and bolt of each captured state.
BATTERY_STATES = {
    "absent": (None, False, False),
    "low": (0, True, False),
    "steps-1": (1, False, False),
    "steps-2": (2, False, False),
    "steps-3": (3, False, False),
    "steps-4": (4, False, False),
    "steps-5": (5, False, False),
    "charging-low": (0, True, True),
    "charging-steps-3": (3, False, True),
    "charging-steps-5": (5, False, True),
}


def rect_pixels(rect: Rect) -> set[tuple[int, int]]:
    x, y, width, height = rect
    return {(column, row) for column in range(x, x + width) for row in range(y, y + height)}


def expected_battery(
    icon: BatteryPixels, state: str, colours: dict[str, int]
) -> dict[tuple[int, int], int]:
    """Map every painted pixel of a state to its colour; the rest is the bar."""
    steps, low, charging = BATTERY_STATES[state]
    painted: dict[tuple[int, int], int] = {}
    if steps is None:
        return painted
    x, y, width, height = icon.body
    shell = colours["danger" if low else "ink"]
    outline = rect_pixels(icon.body) - rect_pixels((x + 1, y + 1, width - 2, height - 2))
    corners = {(x, y), (x + width - 1, y), (x, y + height - 1), (x + width - 1, y + height - 1)}
    for point in (outline - corners) | rect_pixels(icon.cap):
        painted[point] = shell
    bar_width, bar_height = icon.bar_size
    for bar_x in icon.bar_x[:steps]:
        for point in rect_pixels((bar_x, y + 2, bar_width, bar_height)):
            painted[point] = colours["ink"]
    if low:
        for point in rect_pixels(icon.low_stem) | rect_pixels(icon.low_dot):
            painted[point] = colours["danger"]
    if charging:
        left, top = icon.bolt_origin
        for row, line in enumerate(icon.bolt):
            for column, mark in enumerate(line):
                if mark == "#":
                    painted[left + column, top + row] = colours["ink"]
    return painted


def capture_status_bar(  # noqa: PLR0913, PLR0917 -- independent native boundary inputs.
    launcher_native: Path,
    catalogue: tuple[Path, Path],
    glyphs: Path,
    directory: Path,
    screen: tuple[int, int],
    theme: str,
) -> None:
    """Capture every battery state with the status clock, then the locked status bar."""
    applications, icons = catalogue
    width, height = screen
    result = run_process(
        [
            str(launcher_native / "launcher-screens"),
            str(width),
            str(height),
            str(launcher_native / "launcher"),
            str(SCREENS / "keypad.xkb"),
            str(directory / "session.sock"),
            str(directory),
            str(applications),
            str(icons),
            theme,
            "1",
            "battery",
            "en",
        ],
        name=f"actual Launcher status bar at {width}x{height} ({theme})",
        env=os.environ | {"FPLINUX_NATIVE_GLYPHS": str(glyphs / "usr/share/fplinux/glyphs")},
        timeout=30,
    )
    (directory / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    retain_capture(directory, f"launcher-battery-{width}x{height}-{theme}")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "launcher-battery: every level, low and charging state drawn" in result.stdout


STATUS_COLOURS = {
    "light": {"bar": "#EFEFEF", "ink": "#181818", "danger": "#B51418"},
    "dark": {"bar": "#212021", "ink": "#DEDFDE", "danger": "#FFB6AD"},
}


@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize(
    "screen", list(BATTERY_PIXELS), ids=lambda screen: "x".join(map(str, screen))
)
def test_actual_battery_icon_pixels(  # noqa: PLR0913, PLR0917 -- explicit matrix inputs.
    launcher_native: Path,
    launcher_catalogue: tuple[Path, Path],
    ui_icon_resources: Path,
    tmp_path: Path,
    screen: tuple[int, int],
    theme: str,
) -> None:
    """Every state's battery is drawn with whole pixels in the theme's colours only.

    The outline leaves its corner pixels out, the bars keep 1 px from it on
    all sides and between each other, and the bolt and "!" are crisp.
    """
    capture_status_bar(
        launcher_native, launcher_catalogue, ui_icon_resources, tmp_path, screen, theme
    )
    width, height = screen
    packed = {name: rgb565(colour) for name, colour in STATUS_COLOURS[theme].items()}
    icon = BATTERY_PIXELS[screen]
    # From one bar-coloured column left of the bolt to the right screen edge.
    left = icon.bolt_origin[0] - 1
    for state in BATTERY_STATES:
        frame = (tmp_path / f"launcher-battery-{state}.rgb565").read_bytes()
        assert len(frame) == width * height * 2
        painted = expected_battery(icon, state, packed)
        wrong = [
            (x, y, hex(pixel(frame, width, x, y)))
            for y in range(STATUS_ROWS[screen])
            for x in range(left, width)
            if pixel(frame, width, x, y) != painted.get((x, y), packed["bar"])
        ]
        assert not wrong, f"{state}: {len(wrong)} pixels differ, first {wrong[:8]}"


def ink_rows(frame: bytes, width: int, columns: range, rows: int, bar: int) -> list[int]:
    """Status rows with anything but the bar colour in columns."""
    return [y for y in range(rows) if any(pixel(frame, width, x, y) != bar for x in columns)]


@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize(
    "screen", list(BATTERY_PIXELS), ids=lambda screen: "x".join(map(str, screen))
)
def test_actual_status_bar_items_are_centred_above_the_divider(  # noqa: PLR0913, PLR0917 -- explicit matrix inputs.
    launcher_native: Path,
    launcher_catalogue: tuple[Path, Path],
    ui_icon_resources: Path,
    tmp_path: Path,
    screen: tuple[int, int],
    theme: str,
) -> None:
    """The clock digits, the lock, the battery and the bolt share one vertical rule.

    Each item's ink has as many bar rows above it as above the divider below
    it, or one more below when their sum is odd; the divider is not space.
    """
    capture_status_bar(
        launcher_native, launcher_catalogue, ui_icon_resources, tmp_path, screen, theme
    )
    width, _ = screen
    rows = STATUS_ROWS[screen]
    bar = rgb565(STATUS_COLOURS[theme]["bar"])
    icon = BATTERY_PIXELS[screen]
    battery_left = icon.body[0]
    items = {
        # The clock text is centred on the screen, well clear of both slots.
        "clock digits": ("steps-3", range(width // 2 - 20, width // 2 + 20)),
        "battery": ("steps-3", range(battery_left, width)),
        "bolt": ("charging-steps-3", range(icon.bolt_origin[0], battery_left)),
        "lock": ("status-locked", range(width // 4)),
    }
    for item, (state, columns) in items.items():
        name = state if state == "status-locked" else f"battery-{state}"
        frame = (tmp_path / f"launcher-{name}.rgb565").read_bytes()
        ink = ink_rows(frame, width, columns, rows, bar)
        assert ink, f"{item} is not drawn"
        above, below = ink[0], rows - 1 - ink[-1]
        assert below - above in (0, 1), f"{item}: {above} rows above, {below} below"


def test_actual_launcher_marquee_finishes_and_restarts_on_focus(
    launcher_native: Path, launcher_catalogue: tuple[Path, Path], tmp_path: Path
) -> None:
    """The real grid's long title stops after one pass and stays stopped while hidden."""
    applications, icons = launcher_catalogue
    entry = applications / "org.test.app01.desktop"
    entry.write_text(
        entry.read_text(encoding="utf-8").replace(
            "Name=App 01", "Name=A long application title to read"
        ),
        encoding="utf-8",
    )
    result = run_process(
        [
            str(launcher_native / "launcher-screens"),
            "128",
            "128",
            str(launcher_native / "launcher"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(applications),
            str(icons),
            "light",
            "1",
            "marquee",
            "en",
        ],
        name="actual Launcher grid title completes one marquee pass",
        timeout=30,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    retain_capture(tmp_path, "launcher-marquee-128x128-light-en")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "one complete pass; idle stays still; new focus restarts; hiding stops" in result.stdout


def test_actual_launcher_answered_ring_stays_until_the_owner_replies(
    launcher_native: Path, launcher_catalogue: tuple[Path, Path], tmp_path: Path
) -> None:
    """Stop keeps the ring until its result; a slow reply covers it, a new ring replaces it."""
    applications, icons = launcher_catalogue
    result = run_process(
        [
            str(launcher_native / "launcher-screens"),
            "176",
            "220",
            str(launcher_native / "launcher"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(applications),
            str(icons),
            "light",
            "1",
            "ring",
            "en",
        ],
        name="actual Launcher answers ring actions through the owner",
        timeout=30,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    retain_capture(tmp_path, "launcher-ring-176x220-light-en")
    assert result.returncode == 0, result.stdout + result.stderr
    assert [line.split(":", 1)[0] for line in result.stdout.splitlines()] == [
        "launcher-ring-fast",
        "launcher-ring-slow",
        "launcher-ring-failed",
        "launcher-ring-slow-failed",
        "launcher-ring-superseded-early",
        "launcher-ring-superseded-late",
    ]


@pytest.mark.parametrize("view", ["home", "grid", "list"])
@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("screen", [(128, 128), (320, 240)])
def test_system_notice_reads_and_closes_over_each_launcher_view(  # noqa: PLR0913, PLR0917 -- native boundary inputs.
    launcher_native: Path,
    launcher_catalogue: tuple[Path, Path],
    tmp_path: Path,
    view: str,
    theme: str,
    screen: tuple[int, int],
) -> None:
    """The same readable notice preserves the bars and returns to its view."""
    applications, icons = launcher_catalogue
    width, height = screen
    result = run_process(
        [
            str(launcher_native / "launcher-screens"),
            str(width),
            str(height),
            str(launcher_native / "launcher"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(applications),
            str(icons),
            theme,
            "1",
            f"notice-{view}",
            "en",
        ],
        name=f"system notice over {view} at {width}x{height} ({theme})",
        timeout=30,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    retain_capture(tmp_path, f"launcher-notice-{view}-{width}x{height}-{theme}")
    assert result.returncode == 0, result.stdout + result.stderr
    before = (tmp_path / "notice-before.rgb565").read_bytes()
    shown = (tmp_path / "notice-shown.rgb565").read_bytes()
    closed = (tmp_path / "notice-closed.rgb565").read_bytes()
    assert len(before) == len(shown) == len(closed) == width * height * 2
    assert shown != closed
    # The scrim leaves both system bars readable; their outer edge stays put.
    assert pixel(shown, width, 0, 0) == pixel(before, width, 0, 0)
    assert pixel(shown, width, 0, height - 1) == pixel(before, width, 0, height - 1)
    title = "Too many open apps"
    body = "Close an application in Tasks, then try again. Your open applications were kept."
    initial = {
        label["text"]: label for label in observed_frame(tmp_path, "notice-shown")["labels"]
    }
    assert 0 < initial[title]["bounds"][3] <= (25 if width == 128 else 28)
    _, body_top, _, body_height = initial[body]["bounds"]
    assert body_top >= (16 if width == 128 else 19)
    assert body_top + body_height <= height - (16 if width == 128 else 22)


# Status bar and title band, soft-key bar, and the level-row detail pitch.
QUICK_SETTINGS_ROWS = {
    (128, 128): ((41, 16), 36),
    (128, 160): ((38, 18), 35),
    (176, 220): ((45, 24), 38),
    (240, 320): ((48, 32), 43),
    (320, 240): ((47, 22), 43),
}


# The soft-key legends of the Settings Bluetooth screens, as left|centre|right.
APPLICATION_LEGENDS = ("|Open|Back", "|Turn on|Back", "|Turn off|Back", "||", "||Close", "||Back")


@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize(
    "screen", list(QUICK_SETTINGS_ROWS), ids=lambda screen: "x".join(map(str, screen))
)
def test_actual_launcher_bars_carry_application_legends_over_one_status_bar(  # noqa: PLR0913, PLR0917 -- explicit matrix inputs.
    launcher_native: Path,
    launcher_catalogue: tuple[Path, Path],
    ui_icon_resources: Path,
    tmp_path: Path,
    screen: tuple[int, int],
    theme: str,
) -> None:
    """Each forwarded legend set changes only the soft-key bar the launcher draws."""
    applications, icons = launcher_catalogue
    width, height = screen
    result = run_process(
        [
            str(launcher_native / "launcher-screens"),
            str(width),
            str(height),
            str(launcher_native / "launcher"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(applications),
            str(icons),
            theme,
            "1",
            "application",
            "en",
            *APPLICATION_LEGENDS,
        ],
        name=f"actual Launcher bars over an application at {width}x{height} ({theme})",
        env=os.environ
        | {"FPLINUX_NATIVE_GLYPHS": str(ui_icon_resources / "usr/share/fplinux/glyphs")},
        timeout=30,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    retain_capture(tmp_path, f"launcher-application-{width}x{height}-{theme}")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "launcher-application: the bars carry each application's legends" in result.stdout
    (_, softkeys), _ = QUICK_SETTINGS_ROWS[screen]
    frames = [
        (tmp_path / f"application-{index}.rgb565").read_bytes()
        for index in range(1, len(APPLICATION_LEGENDS) + 1)
    ]
    row = width * 2
    status_rows = {frame[: row * STATUS_ROWS[screen]] for frame in frames}
    bodies = {frame[row * STATUS_ROWS[screen] : row * (height - softkeys)] for frame in frames}
    bars = {frame[row * (height - softkeys) :] for frame in frames}
    assert len(status_rows) == 1
    assert len(bodies) == 1
    assert len(bars) == len(APPLICATION_LEGENDS)
