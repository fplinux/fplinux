# SPDX-License-Identifier: GPL-2.0-only
"""Real Settings, UI runtime and Wayland frames with controlled session replies."""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.host_tool.gui.list_rows import ListArea, frame_pixels, sequence_faults
from tests.host_tool.gui.private_bus import private_bus
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SCREENS = ROOT / "site/scripts/screens"
# Title band, value row pitch and status plus soft-key bars of each profile:
# a value under its label takes the detail pitch, one beside it in landscape
# the ordinary pitch.
ROOT_ROWS = {
    (128, 128): (25, 36, 32),
    (128, 160): (22, 35, 34),
    (176, 220): (26, 38, 43),
    (240, 320): (28, 43, 52),
    (320, 240): (28, 32, 41),
}


def test_actual_settings_about_never_leaves_the_last_line_partly_hidden(
    settings_native: Path, tmp_path: Path
) -> None:
    """Scroll the real About form through the end on the landscape profile."""
    source = ROOT / "alpine/aports/fplinux-brand"
    for name in (
        "generate-brand.py",
        "fplinux-mark-light.svg",
        "fplinux-mark-dark.svg",
        "fplinux-lockup-light.svg",
        "fplinux-lockup-dark.svg",
    ):
        shutil.copyfile(source / name, tmp_path / name)
    shutil.copyfile(ROOT / "alpine/aports/fplinux-icons/svgraster.py", tmp_path / "svgraster.py")
    brand = tmp_path / "brand"
    run_process(
        [
            sys.executable,
            str(tmp_path / "generate-brand.py"),
            "--profile",
            str(ROOT / "lib/fplinux/fplinux-ui-profile.json"),
            "--out",
            str(brand),
        ],
        name="generate the installed About logo from the real brand masters",
        env=os.environ | {"NODE_PATH": "/opt/quality/node-tools/node_modules"},
        timeout=30,
        check=True,
    )
    result = run_process(
        [
            str(settings_native / "settings-screens"),
            str(settings_native / "settings"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(ROOT / "alpine/aports/fplinux-language"),
            "about-edges",
        ],
        name="measure actual Settings About scrolling at 320x240",
        env={
            "FPLINUX_SETTINGS_TEST_WIDTH": "320",
            "FPLINUX_SETTINGS_TEST_HEIGHT": "240",
            "FPLINUX_SETTINGS_TEST_LOCALE": "en",
            "FPLINUX_SETTINGS_TEST_THEME": "dark",
            "FPLINUX_SETTINGS_TEST_SYSTEM_NAME": "FPLinux Alpine system",
            "FPLINUX_SETTINGS_TEST_ABOUT": "1",
            "FPLINUX_SETTINGS_TEST_BRAND": str(brand),
        },
        timeout=20,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / "settings-about-edges",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "settings-about: last text line is whole on every downward step" in result.stdout


@pytest.fixture(scope="module")
def settings_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path, ui_icon_resources: Path
) -> Iterator[Path]:
    """Build the real app and peer against the shared native UI inputs."""
    with c_build_directory(tmp_path_factory, "settings-native") as directory:
        run_process(
            ["sh", str(SCREENS / "build-settings.sh"), str(directory), str(ui_host_inputs)],
            name="build the actual Settings and native session peer",
            timeout=180,
            check=True,
        )
        (directory / "icons").symlink_to(ui_icon_resources, target_is_directory=True)
        yield directory


def test_actual_settings_applied_storage_errors_commit_before_ack_and_keep_notice(
    settings_native: Path, tmp_path: Path
) -> None:
    """A session peer injects storage outcomes; the actual app paints and handles input."""
    result = run_process(
        [
            str(settings_native / "settings-screens"),
            str(settings_native / "settings"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(ROOT / "alpine/aports/fplinux-language"),
        ],
        name="apply Settings theme and locale storage failures through actual UI frames",
        timeout=20,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / "settings-storage-frames",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "settings-warning: Cancel leaves the live theme and sends no change\n"
        "settings-success: live Light widgets and pixels precede REDRAW_ACK\n"
        "settings-theme-error: applied Dark frame acknowledges ENOSPC; notice waits for Close\n"
        "settings-date: real editors retain date, time and focus through pending Home, refusal "
        "and cancelled close; live-clock reply completes Save\n"
        "settings-locale-error: applied Русский frame acknowledges EIO; "
        "rebuilt notice waits for Закрыть\n"
    )
    frames = [
        (tmp_path / f"settings-{name}.rgb565").read_bytes()
        for name in ("theme-saved", "theme-error", "locale-error", "locale-closed")
    ]
    assert all(len(frame) == 240 * 268 * 2 for frame in frames)
    assert len(set(frames)) == 4


@pytest.mark.parametrize(
    "screen",
    [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)],
    ids=["128-128", "128-160", "176-220", "240-320", "320-240"],
)
@pytest.mark.parametrize("locale", ["en", "ru"])
@pytest.mark.parametrize("theme", ["light", "dark"])
def test_actual_settings_root_values_fit_scroll_and_follow_current_replies(
    settings_native: Path, tmp_path: Path, screen: tuple[int, int], locale: str, theme: str
) -> None:
    """Actual glyph rows keep values readable and refresh after live setting replies."""
    width, height = screen
    result = run_process(
        [
            str(settings_native / "settings-screens"),
            str(settings_native / "settings"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(ROOT / "alpine/aports/fplinux-language"),
            "root-values",
        ],
        name=f"measure actual Settings root values at {width}x{height} in {locale}/{theme}",
        env={
            "FPLINUX_SETTINGS_TEST_WIDTH": str(width),
            "FPLINUX_SETTINGS_TEST_HEIGHT": str(height),
            "FPLINUX_SETTINGS_TEST_LOCALE": locale,
            "FPLINUX_SETTINGS_TEST_THEME": theme,
            "FPLINUX_SETTINGS_TEST_SYSTEM_NAME": (
                "FPLinux release with a long system name that must remain readable "
                "in every profile"
            ),
        },
        timeout=20,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"settings-root-values-{width}x{height}-{locale}-{theme}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "settings-root-values: real glyphs and current values fit; landscape shares a right "
        "column; long system name scrolls; replies refresh retained values\n"
    )
    # Below the title band, value rows at the detail pitch fill the surface
    # less its status and soft-key bars.
    band, pitch, bars = ROOT_ROWS[screen]
    area = ListArea(
        width=width,
        top=band,
        bottom=height - bars,
        pitch=pitch,
        edge=(0, 2),
        divider=0xBE39 if theme == "light" else 0x3A29,
        highlight=0x3B0E if theme == "light" else 0xAE7B,
    )
    names = ("display", "language", "input", "about", "up-input", "up-language", "up-display")
    frames = {
        name: frame_pixels((tmp_path / f"settings-root-{name}.rgb565").read_bytes(), width)
        for name in names
    }
    assert sequence_faults(frames, area) == []


@pytest.mark.parametrize(
    "screen",
    [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)],
    ids=["128-128", "128-160", "176-220", "240-320", "320-240"],
)
@pytest.mark.parametrize("theme", ["light", "dark"])
def test_actual_settings_bluetooth_switch_follows_bluez_with_whole_rows(
    settings_native: Path, tmp_path: Path, screen: tuple[int, int], theme: str
) -> None:
    """The real switch against a BlueZ fake: live state, a slow and a refused change.

    The fake replaces bluetoothd at its D-Bus boundary on a private bus.
    """
    width, height = screen
    with private_bus() as bus:
        result = run_process(
            [
                str(settings_native / "settings-screens"),
                str(settings_native / "settings"),
                str(SCREENS / "keypad.xkb"),
                str(tmp_path / "session.sock"),
                str(tmp_path),
                str(ROOT / "alpine/aports/fplinux-language"),
                "bluetooth",
            ],
            name=f"switch Bluetooth in the actual Settings at {width}x{height} ({theme})",
            env=bus.environment()
            | {
                "FPLINUX_SETTINGS_TEST_WIDTH": str(width),
                "FPLINUX_SETTINGS_TEST_HEIGHT": str(height),
                "FPLINUX_SETTINGS_TEST_LOCALE": "en",
                "FPLINUX_SETTINGS_TEST_THEME": theme,
            },
            timeout=30,
        )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"settings-bluetooth-{width}x{height}-{theme}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "settings-bluetooth: the switch follows BlueZ, waits for slow changes, reports "
        "refusals and goes with bluetoothd\n"
    )
    band, pitch, bars = ROOT_ROWS[screen]
    area = ListArea(
        width=width,
        top=band,
        bottom=height - bars,
        pitch=pitch,
        edge=(0, 2),
        divider=0xBE39 if theme == "light" else 0x3A29,
        highlight=0x3B0E if theme == "light" else 0xAE7B,
    )
    names = [
        *(f"root-{row}" for row in ("display", "bluetooth", "language", "input", "about")),
        *(f"root-up-{row}" for row in ("input", "language", "bluetooth")),
    ]
    frames = {
        name: frame_pixels((tmp_path / f"settings-bluetooth-{name}.rgb565").read_bytes(), width)
        for name in names
    }
    assert sequence_faults(frames, area) == []
    # The switch screen's only row has no divider; its states differ.
    switch = {
        (tmp_path / f"settings-bluetooth-{name}.rgb565").read_bytes()
        for name in ("off", "on", "unavailable")
    }
    assert len(switch) == 3
