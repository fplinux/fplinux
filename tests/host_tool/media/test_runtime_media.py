# SPDX-License-Identifier: GPL-2.0-only
"""Actual guest resource decoders and worker with a controlled UI PCM boundary."""

from __future__ import annotations

from pathlib import Path

from tests import ROOT
from tests.c_harness import compile_c
from tests.host_tool.media.test_media_decode import (
    native_mpg123 as native_mpg123,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.host_tool.media.test_media_decode import (
    tone_mp3 as tone_mp3,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.host_tool.media.test_midi_render import (
    native_midi as native_midi,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.process import run_process


def test_guest_worker_short_writes_heard_eof_pause_seek_and_midi_resources(
    tmp_path: Path,
    native_mpg123: Path,
    native_midi: Path,
    lvgl_library: tuple[Path, Path],
    tone_mp3: bytes,
) -> None:
    executable = tmp_path / "runtime-media"
    headers, _ = lvgl_library
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            f"-I{ROOT / 'include'}",
            f"-I{ROOT / 'include/fplinux'}",
            "-isystem",
            str(headers),
            "-DLV_KCONFIG_IGNORE",
            "-DLV_LVGL_H_INCLUDE_SYSTEM",
            f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
            f"-I{native_mpg123 / 'src/include'}",
            "-isystem",
            str(native_midi / "include"),
            f'-DFPLINUX_MIDI_SOUNDFONT="{native_midi / "bank/TimGM6mb.sf2"}"',
            str(Path(__file__).with_name("runtime-media.c")),
            str(Path(__file__).parent / "fixtures/runtime-media/ui-boundary.c"),
            str(ROOT / "lib/fplinux-runtime-media/fplinux-runtime-media.c"),
            str(ROOT / "lib/fplinux-media/fplinux-media.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            str(ROOT / "lib/fplinux-midi/fplinux-midi.c"),
            str(ROOT / "lib/fplinux-midi/fplinux-midi-smf.c"),
            str(ROOT / "lib/fplinux-midi/fplinux-midi-tone.c"),
            str(ROOT / "lib/fplinux-midi/fplinux-midi-synth.c"),
            str(native_mpg123 / "src/libmpg123/.libs/libmpg123.a"),
            "-lm",
        ],
        name="link the actual guest broker and decoders with controlled UI output",
    )
    fixture = tmp_path / "tone-vbr.mp3"
    fixture.write_bytes(tone_mp3)
    result = run_process(
        [str(executable), str(fixture)],
        name="exercise guest resource worker and actual PCM",
        timeout=30,
        check=True,
    )
    assert result.stdout == "runtime media: ok\n"
