# SPDX-License-Identifier: GPL-2.0-only
"""MRE parent adapter and real worker/decoders with a controlled UI PCM boundary."""

from __future__ import annotations

from pathlib import Path

from tests import ROOT
from tests.c_harness import compile_c
from tests.host_tool.media.decoder_fixtures import (
    media_decoder_args as media_decoder_args,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.host_tool.media.decoder_fixtures import (
    native_mpg123 as native_mpg123,  # noqa: PLC0414 -- pytest fixture dependency.
)
from tests.host_tool.media.decoder_fixtures import (
    tone_mp3 as tone_mp3,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.host_tool.media.test_midi_render import (
    native_midi as native_midi,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.process import run_process


def test_mre_parent_midi_loops_pause_gain_mp3_heard_eof_and_guest_cleanup(
    tmp_path: Path,
    media_decoder_args: list[str],
    native_midi: Path,
    lvgl_library: tuple[Path, Path],
    tone_mp3: bytes,
) -> None:
    executable = tmp_path / "mre-audio-broker"
    headers, _ = lvgl_library
    aport = ROOT / "alpine/aports/fplinux-mre"
    boundary = ROOT / "tests/host_tool/media/fixtures/runtime-media"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            f"-I{aport}",
            f"-I{boundary}",
            f"-I{ROOT / 'include'}",
            f"-I{ROOT / 'include/fplinux'}",
            "-isystem",
            str(headers),
            "-DLV_KCONFIG_IGNORE",
            "-DLV_LVGL_H_INCLUDE_SYSTEM",
            f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
            "-isystem",
            str(native_midi / "include"),
            f'-DFPLINUX_MIDI_SOUNDFONT="{native_midi / "bank/TimGM6mb.sf2"}"',
            str(Path(__file__).with_name("audio-broker.c")),
            str(aport / "brokers/audio-broker.c"),
            str(boundary / "ui-boundary.c"),
            str(ROOT / "lib/fplinux-runtime-media/fplinux-runtime-media.c"),
            str(ROOT / "lib/fplinux-midi/fplinux-midi.c"),
            str(ROOT / "lib/fplinux-midi/fplinux-midi-smf.c"),
            str(ROOT / "lib/fplinux-midi/fplinux-midi-tone.c"),
            str(ROOT / "lib/fplinux-midi/fplinux-midi-synth.c"),
            *media_decoder_args,
        ],
        name="link the MRE parent adapter with the actual worker and decoders",
    )
    fixture = tmp_path / "tone-vbr.mp3"
    fixture.write_bytes(tone_mp3)
    result = run_process(
        [str(executable), str(fixture)],
        name="exercise MRE parent audio and actual decoded PCM",
        timeout=60,
        check=True,
    )
    assert result.stdout == "MRE audio broker: ok\n"
