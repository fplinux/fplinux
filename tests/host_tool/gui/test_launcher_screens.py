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


def pixel(frame: bytes, width: int, x: int, y: int) -> int:
    return int(struct.unpack_from("<H", frame, 2 * (y * width + x))[0])


@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize(
    "geometry",
    [
        LauncherGeometry((128, 128), 3, 20, 46, 32, 52, 4, 16),
        LauncherGeometry((128, 160), 4, 22, 47, 32, 52, 4, 16),
        LauncherGeometry((176, 220), 5, 26, 55, 36, 60, 6, 20),
        LauncherGeometry((240, 320), 6, 36, 66, 44, 70, 8, 24),
        LauncherGeometry((320, 240), 5, 30, 61, 40, 66, 6, 24),
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
    accent, background, ring = (
        (0x3B0E, 0xF7BE, 0xFFFF) if theme == "light" else (0xAE7B, 0x1082, 0x09A8)
    )
    for view, pitch, centre in (
        ("list", geometry.row_pitch, geometry.row_centre),
        ("tasks", geometry.task_pitch, geometry.task_centre),
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
        (128, 160): (16, 144),
        (176, 220): (18, 196),
        (240, 320): (20, 288),
        (320, 240): (20, 216),
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
    if count <= 8:
        frame = (tmp_path / "launcher-grid-fit.rgb565").read_bytes()
        assert pixel(frame, 320, 46, 67) == 0xF800
        assert pixel(frame, 320, 46, 143) == 0xF800
        assert pixel(frame, 320, 274, 67) == 0xF800
        assert pixel(frame, 320, 16, 63) == 0xF800
        assert pixel(frame, 320, 303, 63) == 0xF800
        red_rows = [y for y in range(46, 216) if pixel(frame, 320, 46, y) == 0xF800]
        assert red_rows == [*range(63, 123), *range(139, 199)]


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
