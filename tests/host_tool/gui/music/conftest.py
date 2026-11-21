# SPDX-License-Identifier: GPL-2.0-only
"""Link Music's native tests with the playback decoders and pinned MIDI bank."""

from __future__ import annotations

import os
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.host_tool.media.decoder_fixtures import (
    media_decoder_args as media_decoder_args,  # noqa: PLC0414 -- pytest fixture dependency.
)
from tests.host_tool.media.decoder_fixtures import (
    native_mpg123 as native_mpg123,  # noqa: PLC0414 -- pytest fixture dependency.
)
from tests.host_tool.media.test_midi_render import (
    native_midi as native_midi,  # noqa: PLC0414 -- pytest fixture dependency.
)

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture(scope="module")
def music_decoder_args(media_decoder_args: list[str], native_midi: Path) -> list[str]:
    midi = ROOT / "lib/fplinux-midi"
    return [
        "-isystem",
        str(native_midi / "include"),
        str(midi / "fplinux-midi.c"),
        str(midi / "fplinux-midi-smf.c"),
        str(midi / "fplinux-midi-tone.c"),
        str(midi / "fplinux-midi-synth.c"),
        *media_decoder_args,
    ]


@pytest.fixture(scope="module")
def music_app_decoder_args(music_decoder_args: list[str]) -> list[str]:
    # The shared UI build already links the production UTF-8 routines.
    utf8 = str(ROOT / "lib/fplinux/fplinux-utf8.c")
    return [argument for argument in music_decoder_args if argument != utf8]


@pytest.fixture(scope="module")
def music_midi_environment(native_midi: Path) -> dict[str, str]:
    return os.environ | {"FPLINUX_MIDI_SOUNDFONT_PATH": str(native_midi / "bank/TimGM6mb.sf2")}
