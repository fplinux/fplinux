# SPDX-License-Identifier: GPL-2.0-only
"""Snake's actual Canvas, Alert, key ownership and pixels on native LVGL.

The session and compositor are controlled peers. Ordinary movement and collision
use the game model; a peer supplies the win outcome to exercise that UI route.
This host integration does not qualify a phone.
"""

from __future__ import annotations

import os
import shutil
import struct
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

HERE = ROOT / "tests/host_tool/gui"


@pytest.fixture(scope="module")
def snake_native(tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path) -> Iterator[Path]:
    """Build the real app and UI with an observer of painted state."""
    with c_build_directory(tmp_path_factory, "snake-native") as directory:
        run_process(
            [
                "sh",
                str(HERE / "snake-native/build.sh"),
                str(ROOT),
                str(directory),
                str(ui_host_inputs),
            ],
            name="build real Snake and native screen-stack peer",
            timeout=180,
            check=True,
        )
        yield directory


@pytest.mark.parametrize("screen", [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)])
@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_snake_alerts_shortcuts_hidden_return_and_fullscreen_frames(
    snake_native: Path, tmp_path: Path, screen: tuple[int, int], theme: str, locale: str
) -> None:
    """One root survives every modal; releases cannot reapply the shortcut."""
    width, height = screen
    scenario = "pause-back" if screen == (128, 128) else "results"
    result = run_process(
        [
            str(snake_native / "snake-screens"),
            str(snake_native / "snake"),
            str(ROOT / "site/scripts/screens/keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(width),
            str(height),
            theme,
            locale,
            scenario,
        ],
        name=f"actual Snake screen stack at {width}x{height} ({theme}, {locale})",
        env={"FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
        timeout=20,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"snake-screens-{width}x{height}-{theme}-{locale}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.endswith(
        "snake-screens: permanent Canvas; Alert commands; Space press/release; "
        "paused input; hidden return; Back closes\n"
    )
    states = ["start", "playing", "paused"]
    if scenario == "results":
        states.extend(["loss", "win"])
    frames = [(tmp_path / f"snake-{state}.rgb565").read_bytes() for state in states]
    for frame in frames:
        assert len(frame) == width * height * 2
        assert len(set(struct.iter_unpack("<H", frame))) > 8
    assert len(set(frames)) == len(states)
    top = {(128, 128): 16, (128, 160): 16, (176, 220): 18, (240, 320): 20, (320, 240): 20}[screen]
    playing = tuple(value for (value,) in struct.iter_unpack("<H", frames[1]))
    bar, border = (0x2104, 0x3A29) if theme == "dark" else (0xEF7D, 0xBE39)
    assert playing[(top - 2) * width] == bar
    assert playing[(top - 1) * width] == border


@pytest.mark.parametrize("theme", ["light", "dark"])
def test_actual_snake_panel_uses_the_profile_divider_pixels(
    snake_native: Path, tmp_path: Path, theme: str
) -> None:
    """A changed existing profile value reaches the app's drawn panel boundary."""
    result = run_process(
        [
            str(snake_native / "snake-screens"),
            str(snake_native / "snake"),
            str(ROOT / "site/scripts/screens/keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            "128",
            "128",
            theme,
            "en",
            "divider",
        ],
        name=f"actual Snake panel with 3px divider ({theme})",
        env={
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
            "FPLINUX_NATIVE_DIVIDER": "3",
        },
        timeout=15,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    pixels = tuple(
        value
        for (value,) in struct.iter_unpack("<H", (tmp_path / "snake-playing.rgb565").read_bytes())
    )
    bar, border = (0x2104, 0x3A29) if theme == "dark" else (0xEF7D, 0xBE39)
    assert pixels[12 * 128] == bar
    assert [pixels[y * 128] for y in range(13, 16)] == [border] * 3
