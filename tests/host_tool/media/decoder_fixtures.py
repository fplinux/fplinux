# SPDX-License-Identifier: GPL-2.0-only
"""Pinned native decoder inputs used by the media and playback tests."""

from __future__ import annotations

import base64
import shlex
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


@pytest.fixture(scope="module")
def native_mpg123(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "native-mpg123") as directory:
        run_process(
            ["sh", str(Path(__file__).with_name("build-media-decoder.sh")), str(directory)],
            name="build the pinned native MP3 decoder",
            timeout=180,
            check=True,
        )
        yield directory / "source"


@pytest.fixture(scope="module")
def media_decoder_args(native_mpg123: Path) -> list[str]:
    ffmpeg = run_process(
        ["pkg-config", "--cflags", "--libs", "libavcodec-fplinux-amr", "libavutil-fplinux-amr"],
        name="locate the pinned native FFmpeg decoder libraries",
        timeout=10,
        check=True,
    )
    return [
        f"-I{native_mpg123 / 'src/include'}",
        str(ROOT / "lib/fplinux-media/fplinux-media.c"),
        str(ROOT / "lib/fplinux-media/fplinux-media-amr-nb.c"),
        str(ROOT / "lib/fplinux/fplinux-utf8.c"),
        str(native_mpg123 / "src/libmpg123/.libs/libmpg123.a"),
        *shlex.split(ffmpeg.stdout),
        "-lm",
    ]


@pytest.fixture(scope="module")
def tone_mp3() -> bytes:
    fixture = Path(__file__).parent / "fixtures/media-decode/tone-vbr.mp3.base64.fixture"
    return base64.b64decode(fixture.read_bytes())
