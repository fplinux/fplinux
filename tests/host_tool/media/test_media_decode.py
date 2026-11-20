# SPDX-License-Identifier: GPL-2.0-only
"""Literal PCM and independent compressed audio vectors through the shared decoders."""

from __future__ import annotations

import base64
import struct
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.media.decoder_fixtures import (
    media_decoder_args as media_decoder_args,  # noqa: PLC0414 -- pytest fixture dependency.
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


IMA_ADPCM_MONO = bytes.fromhex(
    "23010300 12345678 abcdef01 dbde2c00 89abcdef 01234567 ffff1f00 7777ffff 3210ba98"
)
IMA_ADPCM_STEREO = bytes.fromhex(
    "23010300 79fd0700 12345678 abcdef12 abcdef01 543210a0 "
    "dbde2c00 ee032100 89abcdef 11123344 01234567 ffedccaa "
    "ffff1f00 80002700 7777ffff ffff7777 3210ba98 f08c117b"
)


def ima_adpcm_wave(data: bytes, channels: int, *, frames: int, block_frames: int = 17) -> bytes:
    def chunk(name: bytes, body: bytes) -> bytes:
        return name + struct.pack("<I", len(body)) + body + bytes(len(body) % 2)

    block_bytes = 12 * channels
    fmt = struct.pack(
        "<HHIIHHHH",
        0x11,
        channels,
        8000,
        8000 * block_bytes // 17,
        block_bytes,
        4,
        2,
        block_frames,
    )
    body = b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"fact", struct.pack("<I", frames))
    return chunk(b"RIFF", body + chunk(b"data", data) + chunk(b"JUNK", b"end"))


# FFmpeg CLI 8.1.3 decoded the synthetic WAVs with atrim=end_sample=43 and s16le output.
@pytest.mark.parametrize(
    ("channels", "data", "pcm"),
    [
        pytest.param(
            1,
            IMA_ADPCM_MONO,
            bytes.fromhex(
                "230129012c0135013d014b0161015f0185015f0146011401d7005c0075ffd3ff"
                "efffdbde23deebdd86dc9fdbd0d9a5d745d322cb76ce78cfe2d50cda62e25eec"
                "7f00e825ffff0901470318086c1246fcc9cc00800080"
            ),
            id="mono",
        ),
        pytest.param(
            2,
            IMA_ADPCM_STEREO,
            bytes.fromhex(
                "230179fd29016efd2c0167fd350157fd3d0144fd4b011efd6101d6fc5f0108fd"
                "850123fd5f016dfd4601dbfd140124fed70080fe5c008cfe75ffadfed3ffb7fe"
                "efff8afedbdeee0323de2e04ebdd680486dcc0049fdbf004d0d95605a5d7b205"
                "45d31f0622cba30676ce990578cf5b03e2d5d3ff0cdab7f962e237f25eec23e9"
                "7f000ae3e8257eddffff8000090142fe470371f918081def6c12f7d846fc7408"
                "c9cc3f6e0080ff7f0080ff7f"
            ),
            id="stereo",
        ),
    ],
)
def test_ima_adpcm_matches_reference_after_seek_and_repeated_eof(
    media_decode: Path, tmp_path: Path, channels: int, data: bytes, pcm: bytes
) -> None:
    encoded = tmp_path / "tone.wav"
    encoded.write_bytes(ima_adpcm_wave(data, channels, frames=43))
    reference = tmp_path / "tone.s16le"
    reference.write_bytes(pcm)
    result = run_process(
        [str(media_decode), "ima-adpcm", str(encoded), str(reference), str(channels)],
        name="compare IMA ADPCM playback, repeat and mid-block seek against independent PCM",
        timeout=10,
        check=True,
    )
    assert result.stdout == "IMA ADPCM decode: ok\n"


@pytest.mark.parametrize(
    "wav",
    [
        pytest.param(ima_adpcm_wave(IMA_ADPCM_MONO, 1, frames=52), id="fact-overstates-frames"),
        pytest.param(ima_adpcm_wave(IMA_ADPCM_MONO[:-1], 1, frames=43), id="short-final-block"),
        pytest.param(
            ima_adpcm_wave(IMA_ADPCM_MONO, 1, frames=43, block_frames=19),
            id="wrong-frames-per-block",
        ),
    ],
)
def test_ima_adpcm_rejects_inconsistent_chunks(
    media_decode: Path, tmp_path: Path, wav: bytes
) -> None:
    encoded = tmp_path / "invalid.wav"
    encoded.write_bytes(wav)
    run_process(
        [str(media_decode), "reject-wav", str(encoded)],
        name="reject inconsistent IMA ADPCM WAVE chunks",
        timeout=10,
        check=True,
    )


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
