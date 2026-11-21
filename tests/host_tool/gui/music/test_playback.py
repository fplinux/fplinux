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
    tone_mp3 as tone_mp3,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

NATIVE = Path(__file__).parent / "native"
MIDI_NOTE = bytes.fromhex(
    "4d546864 00000006 0000 0001 0060"  # type 0, one track, 96 ticks per quarter note
    "4d54726b 0000000c"
    "00 90 3c 64"  # C4 at time zero
    "60 80 3c 00"  # release after 500 ms at the default tempo
    "00 ff 2f 00"
)


def test_music_worker_folder_queue_seek_and_file_lifetime(
    tmp_path: Path,
    music_decoder_args: list[str],
    music_midi_environment: dict[str, str],
    tone_mp3: bytes,
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
            *music_decoder_args,
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
    (folder / "zz.MIDI").write_bytes(MIDI_NOTE)
    (folder / "zzz.mid").write_bytes(b"broken MIDI")
    (folder / "skip.wav").symlink_to("a.wav")
    (folder / "nested.wav").mkdir()
    (folder / "readme.txt").write_text("Other files remain outside the queue.\n")
    result = run_process(
        [str(executable), str(folder)],
        name="run the Music file worker",
        env=music_midi_environment,
        timeout=10,
        check=True,
    )
    assert result.stdout == "music worker: ok\n"


@pytest.fixture(scope="module")
def music_application(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path, music_app_decoder_args: list[str]
) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "music-state") as directory:
        run_process(
            [
                "sh",
                str(NATIVE / "build-state.sh"),
                str(directory),
                str(ui_host_inputs),
                *music_app_decoder_args,
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


def test_music_document_accepts_midi_and_rejects_invalid_smf(
    music_application: Path, music_midi_environment: dict[str, str], tmp_path: Path
) -> None:
    valid = tmp_path / "note.mid"
    valid.write_bytes(MIDI_NOTE)
    invalid = tmp_path / "broken.midi"
    invalid.write_bytes(b"broken MIDI")
    keypad = ROOT / "tests/host_tool/gui/screens/keypad.xkb"
    with TemporaryDirectory(prefix="fpmusic-") as temporary:
        run_process(
            [
                str(music_application / "music-state-peer"),
                str(music_application / "music"),
                str(Path(temporary) / "session.sock"),
                str(keypad),
                str(valid),
                str(invalid),
                "document",
            ],
            name="open valid and malformed MIDI files through Music's document handler",
            env=music_midi_environment
            | {"HOME": str(tmp_path), "XDG_STATE_HOME": str(tmp_path / "state")},
            timeout=12,
            check=True,
        )
