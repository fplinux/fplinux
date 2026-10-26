# SPDX-License-Identifier: GPL-2.0-only
"""Actual Launcher input, task messages and pixels through a native Wayland peer.

The peer owns the session and test icon tree; application and UI code stay real.
This host integration does not load or qualify a phone.
"""

from __future__ import annotations

import os
import shutil
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SCREENS = ROOT / "site/scripts/screens"


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
