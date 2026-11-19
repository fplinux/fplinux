# SPDX-License-Identifier: GPL-2.0-only
"""Literal PCM and independent MP3 vectors through the actual shared decoders."""

from __future__ import annotations

import base64
import shlex
import struct
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
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
        name="locate the pinned native AMR-NB libraries",
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
def media_decode(
    tmp_path_factory: pytest.TempPathFactory, media_decoder_args: list[str]
) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "media-decode") as directory:
        executable = directory / "media-decode"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include/fplinux'}",
                str(Path(__file__).with_name("media-decode.c")),
                *media_decoder_args,
            ],
            name="link the production shared media decoder and native libraries",
        )
        yield executable


@pytest.fixture(scope="module")
def tone_mp3() -> bytes:
    fixture = Path(__file__).with_name("fixtures") / "media-decode/tone-vbr.mp3.base64.fixture"
    return base64.b64decode(fixture.read_bytes())


@pytest.fixture(scope="module")
def tone_amr_nb() -> bytes:
    fixture = Path(__file__).with_name("fixtures") / "media-decode/tone-amr-nb.base64.fixture"
    return base64.b64decode(fixture.read_bytes())


def test_amr_nb_matches_reference_pcm_at_eof_and_after_seek(
    media_decode: Path, tmp_path: Path, tone_amr_nb: bytes
) -> None:
    encoded = tmp_path / "tone.amr"
    encoded.write_bytes(tone_amr_nb)
    reference = tmp_path / "tone.s16le"
    fixture = Path(__file__).with_name("fixtures") / "media-decode/tone-s16le.base64.fixture"
    reference.write_bytes(base64.b64decode(fixture.read_bytes()))
    result = run_process(
        [str(media_decode), "amr-nb", str(encoded), str(reference)],
        name="compare AMR-NB decode and seek with independent PCM",
        timeout=10,
        check=True,
    )
    assert result.stdout == "AMR-NB decode: ok\n"


def test_media_pcm_gain_seek_metadata_and_real_mp3_eof(
    media_decode: Path, tmp_path: Path, tone_mp3: bytes
) -> None:
    fixture = tmp_path / "tone-vbr.mp3"
    fixture.write_bytes(tone_mp3)
    wrapped = tmp_path / "tone-in-wave.mp3"
    format_body = struct.pack("<HHIIHHHHIHHH", 0x55, 1, 44100, 16000, 1, 0, 12, 1, 0, 1, 1, 0)
    chunks = (
        b"fmt "
        + struct.pack("<I", len(format_body))
        + format_body
        + b"data"
        + struct.pack("<I", len(tone_mp3))
        + tone_mp3
        + bytes(len(tone_mp3) % 2)
        + b"JUNK\x04\x00\x00\x00end!"
    )
    wrapped.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)
    result = run_process(
        [str(media_decode), str(fixture), str(wrapped)],
        name="decode PCM and VBR MP3, including a WAVE-wrapped stream",
        timeout=10,
        check=True,
    )
    assert result.stdout == "media decode: ok\n"


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
            str(Path(__file__).with_name("music-player.c")),
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
