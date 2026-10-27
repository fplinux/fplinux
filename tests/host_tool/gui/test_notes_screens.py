# SPDX-License-Identifier: 0BSD
"""Actual Notes typing, caret, menu Save, background persistence and pixels.

The real application, LVGL screen stack and preferences run against controlled
Wayland and session peers. This host integration does not qualify a phone.
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
def notes_native(tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path) -> Iterator[Path]:
    """Observe painted textarea caret state without replacing any UI behavior."""
    with c_build_directory(tmp_path_factory, "notes-native") as directory:
        run_process(
            [
                "sh",
                str(HERE / "notes-native/build.sh"),
                str(ROOT),
                str(directory),
                str(ui_host_inputs),
            ],
            name="build real Notes and native text/menu peer",
            timeout=180,
            check=True,
        )
        yield directory


@pytest.mark.parametrize("screen", [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)])
@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_notes_save_retains_text_caret_and_editor_then_home_persists(
    notes_native: Path, tmp_path: Path, screen: tuple[int, int], theme: str, locale: str
) -> None:
    """A middle insertion after Save independently verifies the actual caret."""
    width, height = screen
    result = run_process(
        [
            str(notes_native / "notes-screens"),
            str(notes_native / "notes"),
            str(HERE / "notes-native/keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(width),
            str(height),
            theme,
            locale,
        ],
        name=f"actual Notes Save and caret at {width}x{height} ({theme}, {locale})",
        env={"FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
        timeout=20,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"notes-screens-{width}x{height}-{theme}-{locale}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.endswith(
        "notes-screens: clean has no Save; real typing and menu Save retain editor/cursor; "
        "Home saves; reopen reads saved text\n"
    )
    bar_heights = {(128, 128): 32, (128, 160): 32, (176, 220): 42, (240, 320): 52, (320, 240): 44}
    surface_height = height - bar_heights[screen]
    states = ["clean", "dirty", "menu", "saved", "returned", "reopened"]
    frames = [(tmp_path / f"notes-{state}.rgb565").read_bytes() for state in states]
    for frame in frames:
        assert len(frame) == width * surface_height * 2
        assert len(set(struct.iter_unpack("<H", frame))) > 1
    assert frames[0] != frames[1]
    assert frames[1] != frames[2]
    assert frames[2] != frames[3]
    preferences = tmp_path / "state/fplinux/org.fplinux.notes/preferences"
    assert preferences.read_bytes() == b"note=abdc\n"
