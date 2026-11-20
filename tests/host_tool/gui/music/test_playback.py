# SPDX-License-Identifier: GPL-2.0-only
"""Music's track queue, playback worker and saved state in the native application."""

from __future__ import annotations

import os
import struct
import wave
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.media.decoder_fixtures import (
    media_decoder_args as media_decoder_args,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.host_tool.media.decoder_fixtures import (
    native_mpg123 as native_mpg123,  # noqa: PLC0414 -- pytest fixture dependency.
)
from tests.host_tool.media.decoder_fixtures import (
    tone_mp3 as tone_mp3,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

NATIVE = Path(__file__).parent / "native"


def test_music_worker_folder_queue_seek_and_file_lifetime(
    tmp_path: Path, media_decoder_args: list[str], tone_mp3: bytes
) -> None:
    aport = ROOT / "alpine/aports/fplinux-music"
    executable = tmp_path / "music-player"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            "-Wl,--wrap=pread",
            f"-I{aport}",
            f"-I{ROOT / 'include/fplinux'}",
            str(NATIVE / "music-player.c"),
            str(aport / "music-player.c"),
            str(ROOT / "lib/fplinux/fplinux-files-browse.c"),
            *media_decoder_args,
        ],
        name="link the Music worker with actual file browsing and decoders",
    )
    folder = tmp_path / "tracks"
    folder.mkdir()
    samples = struct.pack("<8h", -32768, -16384, 0, 16384, 32767, 1, -1, 8192)
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        52,
        b"WAVE",
        b"fmt ",
        16,
        1,
        1,
        8000,
        16000,
        2,
        16,
        b"data",
        16,
    )
    (folder / "a.wav").write_bytes(header + samples)
    (folder / "z.mp3").write_bytes(tone_mp3)
    (folder / "skip.wav").symlink_to("a.wav")
    (folder / "nested.wav").mkdir()
    (folder / "readme.txt").write_text("Other files remain outside the queue.\n")
    result = run_process(
        [str(executable), str(folder)], name="run the Music file worker", timeout=10, check=True
    )
    assert result.stdout == "music worker: ok\n"


@pytest.fixture(scope="module")
def music_application(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path, native_mpg123: Path
) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "music-state") as directory:
        run_process(
            [
                "sh",
                str(NATIVE / "build-state.sh"),
                str(directory),
                str(ui_host_inputs),
                str(native_mpg123),
            ],
            name="build Music and its native Wayland and session peer",
            timeout=120,
            check=True,
        )
        yield directory


@pytest.mark.parametrize(
    ("repair", "track", "position"),
    [
        pytest.param(False, "a.wav", "1250", id="failed"),
        pytest.param(True, "b.wav", "0", id="retry"),
    ],
)
def test_music_resume_follows_successful_queue_selection(
    music_application: Path, tmp_path: Path, *, repair: bool, track: str, position: str
) -> None:
    tracks = tmp_path / "tracks"
    tracks.mkdir()
    first = tracks / "a.wav"
    with wave.open(str(first), "wb") as audio:
        audio.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        audio.writeframes(b"\0" * 32000)
    broken = tracks / "b.wav"
    broken.write_bytes(b"broken audio")
    replacement = tmp_path / "replacement.wav"
    if repair:
        replacement.write_bytes(first.read_bytes())
    state = tmp_path / "state"
    preferences = state / "fplinux/org.fplinux.music/preferences"
    preferences.parent.mkdir(mode=0o700, parents=True)
    preferences.write_text(f"source={tracks}\ntrack=a.wav\nfolder=1\nposition_ms=1250\n")
    keypad = ROOT / "tests/host_tool/gui/screens/keypad.xkb"
    with TemporaryDirectory(prefix="fpmusic-") as temporary:
        run_process(
            [
                str(music_application / "music-state-peer"),
                str(music_application / "music"),
                str(Path(temporary) / "session.sock"),
                str(keypad),
                str(replacement) if repair else "-",
                str(broken),
            ],
            name="open Music, select the neighboring track, and close",
            env=os.environ | {"HOME": str(tmp_path), "XDG_STATE_HOME": str(state)},
            timeout=12,
            check=True,
        )
    saved = dict(line.split("=", 1) for line in preferences.read_text().splitlines())
    assert saved == {
        "source": str(tracks),
        "track": track,
        "folder": "1",
        "position_ms": position,
        "repeat": "0",
    }
