# SPDX-License-Identifier: GPL-2.0-only
"""The production Music view with real UI fonts, tokens and Wayland frames."""

from __future__ import annotations

import os
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.host_tool.gui.support.rgb565_png import convert
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SCREENS = ROOT / "tests/host_tool/gui/music/native"


@pytest.fixture(scope="module")
def music_view(tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "music-view") as directory:
        run_process(
            ["sh", str(SCREENS / "build-view.sh"), str(directory), str(ui_host_inputs)],
            name="build the actual Music view and native UI capture peer",
            timeout=180,
            check=True,
        )
        yield directory


@pytest.mark.parametrize("size", [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)])
@pytest.mark.parametrize("scene", ["empty", "playing", "paused", "error"])
def test_music_view_keeps_transport_text_and_progress_inside_every_profile(
    music_view: Path, size: tuple[int, int], scene: str
) -> None:
    width, height = size
    previews = {
        (128, 160, "playing"): ("small", "light", "en"),
        (240, 320, "paused"): ("portrait", "dark", "ru"),
        (320, 240, "playing"): ("landscape", "light", "en"),
        (128, 128, "error"): ("error", "dark", "en"),
    }
    preview = previews.get((width, height, scene))
    theme, locale = preview[1:] if preview else ("light", "en")
    with TemporaryDirectory(prefix="fpmview-") as temporary:
        work = Path(temporary)
        capture = work / "view.rgb565"
        result = run_process(
            [
                str(music_view / "music-view-screens"),
                str(music_view / "music-view"),
                str(work / "session.sock"),
                str(capture) if preview else "-",
                str(width),
                str(height),
                theme,
                scene,
                locale,
            ],
            name=f"measure real Music view bounds at {width}x{height} ({scene})",
            env={
                "FPLINUX_MUSIC_TEST_LANGUAGES": str(ROOT / "alpine/aports/fplinux-language"),
                "FPLINUX_MUSIC_TEST_LOCALE_DIR": str(music_view / "locale"),
            },
            timeout=15,
            check=True,
        )
        assert result.stdout == f"music-view: {width}x{height} {theme} {scene} {locale}\n"
        if preview and (log_root := os.environ.get("FPLINUX_LOG_ROOT")):
            # The client surface excludes the shared status and soft-key bars.
            capture_height = len(capture.read_bytes()) // (width * 2)
            output = Path(log_root) / f"music-{preview[0]}.png"
            output.parent.mkdir(parents=True, exist_ok=True)
            convert(width, capture_height, capture, output)
