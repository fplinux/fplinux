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
from tests.host_tool.gui.support.list_rows import ListArea, frame_pixels, sequence_faults
from tests.host_tool.gui.support.private_bus import private_bus
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SCREENS = ROOT / "tests/host_tool/gui/settings/native"
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


@pytest.mark.parametrize("screen", list(ROOT_ROWS))
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_settings_root_sound_summary_follows_live_media_level(
    settings_native: Path, tmp_path: Path, screen: tuple[int, int], locale: str
) -> None:
    """The real root paints the asynchronous Media value before Sound is opened."""
    width, height = screen
    result = run_process(
        [
            str(settings_native / "settings-screens"),
            str(settings_native / "settings"),
            str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(ROOT / "alpine/aports/fplinux-language"),
            "root-sound",
        ],
        name=f"refresh actual Settings root Sound at {width}x{height} in {locale}",
        env={
            "FPLINUX_SETTINGS_TEST_WIDTH": str(width),
            "FPLINUX_SETTINGS_TEST_HEIGHT": str(height),
            "FPLINUX_SETTINGS_TEST_LOCALE": locale,
            "FPLINUX_SETTINGS_TEST_THEME": "dark",
        },
        timeout=20,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"settings-root-sound-{width}x{height}-{locale}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "settings-root-sound: applied snapshots update Media without queries "
        "and refresh before resume\n"
    )


@pytest.mark.parametrize("screen", list(ROOT_ROWS))
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_settings_sound_shows_media_alarm_values_output_choices_and_editors(
    settings_native: Path, tmp_path: Path, screen: tuple[int, int], locale: str
) -> None:
    """Real Sound shows the current Media and Alarm values and opens both editors."""
    width, height = screen
    result = run_process(
        [
            str(settings_native / "settings-screens"),
            str(settings_native / "settings"),
            str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(ROOT / "alpine/aports/fplinux-language"),
            "sound",
        ],
        name=f"measure actual Settings Sound at {width}x{height} in {locale}",
        env={
            "FPLINUX_SETTINGS_TEST_WIDTH": str(width),
            "FPLINUX_SETTINGS_TEST_HEIGHT": str(height),
            "FPLINUX_SETTINGS_TEST_LOCALE": locale,
            "FPLINUX_SETTINGS_TEST_THEME": "dark",
        },
        timeout=20,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"settings-sound-{width}x{height}-{locale}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "settings-sound: Media and Alarm values, supported outputs and both volume editors "
        "are visible\n"
    )


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
            str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
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
            str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
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
            str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
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


@pytest.mark.parametrize("screen", list(ROOT_ROWS))
@pytest.mark.parametrize("locale", ["en", "ru"])
@pytest.mark.parametrize("theme", ["light", "dark"])
def test_actual_settings_sections_use_the_same_value_layout_as_the_root(
    settings_native: Path, tmp_path: Path, screen: tuple[int, int], locale: str, theme: str
) -> None:
    """Real section rows retain portrait details and share the landscape value column."""
    width, height = screen
    result = run_process(
        [
            str(settings_native / "settings-screens"),
            str(settings_native / "settings"),
            str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(ROOT / "alpine/aports/fplinux-language"),
            "section-values",
        ],
        name=f"measure actual Settings section values at {width}x{height} in {locale}/{theme}",
        env={
            "FPLINUX_SETTINGS_TEST_WIDTH": str(width),
            "FPLINUX_SETTINGS_TEST_HEIGHT": str(height),
            "FPLINUX_SETTINGS_TEST_LOCALE": locale,
            "FPLINUX_SETTINGS_TEST_THEME": theme,
            "FPLINUX_SETTINGS_TEST_FIXED_TIME": "1",
            "TZ": "UTC",
        },
        timeout=20,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"settings-section-values-{width}x{height}-{locale}-{theme}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "settings-sections: captured actual value rows and geometry\n"
    rows = (tmp_path / "section-values.txt").read_text(encoding="ascii").splitlines()
    assert [row.split()[:2] for row in rows] == [
        ["display", "0"],
        ["display", "1"],
        ["display", "2"],
        ["display", "3"],
        ["time", "0"],
        ["time", "1"],
        ["input", "0"],
    ]
    for row in rows:
        _, _, *geometry = row.split()
        top, bottom, text_top, text_bottom, value_top, _, text_left, text_right, value_left, _ = (
            map(int, geometry)
        )
        assert bottom - top + 1 == ROOT_ROWS[screen][1], row
        if width > height:
            assert text_top == value_top, row
            assert text_right < value_left, row
        else:
            assert text_bottom < value_top, row
            assert text_left == value_left, row


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
                str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
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
    # The native Bluetooth settings show distinct live power states.
    switch = {
        (tmp_path / f"settings-bluetooth-{name}.rgb565").read_bytes()
        for name in ("off", "on", "unavailable")
    }
    assert len(switch) == 3


@pytest.mark.parametrize("screen", [(128, 128), (240, 320)], ids=["small", "portrait"])
def test_actual_settings_bluetooth_devices_use_live_paths_and_pairing_consent(
    settings_native: Path, tmp_path: Path, screen: tuple[int, int]
) -> None:
    """Real device screens use a private BlueZ peer, without a radio or phone."""
    width, height = screen
    with private_bus() as bus:
        result = run_process(
            [
                str(settings_native / "settings-screens"),
                str(settings_native / "settings"),
                str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
                str(tmp_path / "session.sock"),
                str(tmp_path),
                str(ROOT / "alpine/aports/fplinux-language"),
                "bluetooth-devices",
            ],
            name=f"navigate real Bluetooth devices at {width}x{height}",
            env=bus.environment()
            | {
                "FPLINUX_SETTINGS_TEST_WIDTH": str(width),
                "FPLINUX_SETTINGS_TEST_HEIGHT": str(height),
                "FPLINUX_SETTINGS_TEST_LOCALE": "en",
                "FPLINUX_SETTINGS_TEST_THEME": "dark",
            },
            timeout=30,
        )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"settings-bluetooth-devices-{width}x{height}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "settings-bluetooth-devices: real paths retain focus; pairing, connection and trust "
        "are distinct; hiding releases only owned discovery and the agent\n"
    )
    for state in ("nearby", "pairing", "device", "services"):
        assert (tmp_path / f"settings-bluetooth-{state}.rgb565").stat().st_size == (
            width * (height - ROOT_ROWS[screen][2]) * 2
        )


def test_actual_settings_bluetooth_file_pickers_and_worker_complete_and_cancel_transfers(
    settings_native: Path, tmp_path: Path
) -> None:
    """The real UI, pickers and worker exchange bytes over a private OBEX service."""
    home = tmp_path / "home"
    files = home / "Files"
    files.mkdir(parents=True)
    (files / "payload.txt").write_bytes(b"settings payload\n")
    with private_bus() as bus:
        result = run_process(
            [
                str(settings_native / "settings-screens"),
                str(settings_native / "settings"),
                str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
                str(tmp_path / "session.sock"),
                str(tmp_path),
                str(ROOT / "alpine/aports/fplinux-language"),
                "bluetooth-transfers",
            ],
            name="send, receive and cancel through real Settings and shared file pickers",
            env=bus.environment()
            | {
                "HOME": str(home),
                "FPLINUX_SETTINGS_TEST_WIDTH": "240",
                "FPLINUX_SETTINGS_TEST_HEIGHT": "320",
                "FPLINUX_SETTINGS_TEST_LOCALE": "en",
                "FPLINUX_SETTINGS_TEST_THEME": "dark",
                "FPLINUX_SETTINGS_TEST_OBEX_SERVICE": str(settings_native / "bluetooth-service"),
            },
            timeout=30,
        )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / "settings-bluetooth-transfers",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "settings-bluetooth-transfers: shared pickers send bytes, consent publishes "
        "received files, document results return, and close waits for cancellation\n"
    )
    assert (tmp_path / "obex-send/sent-bytes").read_bytes() == b"settings payload\n"
    assert (files / "incoming.txt").read_bytes() == b"fixture"
    assert (tmp_path / "obex-cancel/cancelled").read_bytes() == b"cancelled\n"
    assert {path.name for path in files.iterdir()} == {"payload.txt", "incoming.txt"}


@pytest.mark.parametrize(
    ("choice", "published"),
    [
        ("keep-both", {"incoming.txt": b"original", "incoming (1).txt": b"fixture"}),
        ("replace", {"incoming.txt": b"fixture"}),
        ("cancel", {"incoming.txt": b"original"}),
    ],
)
def test_actual_settings_bluetooth_conflict_publishes_only_the_chosen_files(
    settings_native: Path, tmp_path: Path, choice: str, published: dict[str, bytes]
) -> None:
    """Choose a conflict row or Cancel with the real UI on the smallest profile."""
    home = tmp_path / "home"
    files = home / "Files"
    files.mkdir(parents=True)
    (files / "incoming.txt").write_bytes(b"original")
    with private_bus() as bus:
        result = run_process(
            [
                str(settings_native / "settings-screens"),
                str(settings_native / "settings"),
                str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
                str(tmp_path / "session.sock"),
                str(tmp_path),
                str(ROOT / "alpine/aports/fplinux-language"),
                f"bluetooth-conflict-{choice}",
            ],
            name=f"choose {choice} for an existing Bluetooth destination",
            env=bus.environment()
            | {
                "HOME": str(home),
                "FPLINUX_SETTINGS_TEST_WIDTH": "128",
                "FPLINUX_SETTINGS_TEST_HEIGHT": "128",
                "FPLINUX_SETTINGS_TEST_LOCALE": "en",
                "FPLINUX_SETTINGS_TEST_THEME": "dark",
                "FPLINUX_SETTINGS_TEST_OBEX_SERVICE": str(settings_native / "bluetooth-service"),
            },
            timeout=30,
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert {path.name: path.read_bytes() for path in files.iterdir()} == published
