# SPDX-License-Identifier: GPL-2.0-only
"""Render literal SMF and ToneControl vectors with the pinned synth and bank."""

from __future__ import annotations

import json
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


@pytest.fixture(scope="module")
def native_midi(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "native-midi") as directory:
        run_process(
            ["sh", str(Path(__file__).with_name("build-midi-renderer.sh")), str(directory)],
            name="build the pinned render-only native synth",
            timeout=300,
            check=True,
        )
        yield directory


@pytest.fixture(scope="module")
def midi_renderers(
    tmp_path_factory: pytest.TempPathFactory, native_midi: Path
) -> Iterator[tuple[Path, Path]]:
    with c_build_directory(tmp_path_factory, "midi-render") as directory:
        outputs = (directory / "midi-render", directory / "missing-bank")
        bank = native_midi / "bank/TimGM6mb.sf2"
        for executable, soundfont in zip(outputs, (bank, directory / "absent.sf2"), strict=True):
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    "-Wl,--wrap=tsf_load_memory",
                    "-Wl,--wrap=tsf_copy",
                    "-Wl,--wrap=tsf_close",
                    "-Wl,--wrap=tsf_set_max_voices",
                    "-Wl,--wrap=tsf_channel_set_presetnumber",
                    "-Wl,--wrap=mmap",
                    "-Wl,--wrap=munmap",
                    f"-I{ROOT / 'include/fplinux'}",
                    f"-I{ROOT / 'lib/fplinux-midi'}",
                    "-isystem",
                    str(native_midi / "include"),
                    f'-DFPLINUX_MIDI_SOUNDFONT="{soundfont}"',
                    str(Path(__file__).with_name("midi-render.c")),
                    str(ROOT / "lib/fplinux-midi/fplinux-midi.c"),
                    str(ROOT / "lib/fplinux-midi/fplinux-midi-smf.c"),
                    str(ROOT / "lib/fplinux-midi/fplinux-midi-tone.c"),
                    str(ROOT / "lib/fplinux-midi/fplinux-midi-synth.c"),
                    "-lm",
                ],
                name="link the production MIDI renderer with the actual synth",
            )
        yield outputs


def test_midi_sample_time_seek_loop_resource_limits_and_actual_pcm(
    midi_renderers: tuple[Path, Path],
) -> None:
    result = run_process(
        [str(midi_renderers[0])],
        name="render SMF and ToneControl through the shared API",
        timeout=30,
        check=True,
    )
    assert result.stdout == "midi render: ok\n"


def test_midi_missing_bank_reports_failure_before_playing(
    midi_renderers: tuple[Path, Path],
) -> None:
    result = run_process(
        [str(midi_renderers[1]), "missing-bank"],
        name="reject a MIDI start without its sound bank",
        timeout=10,
        check=True,
    )
    assert result.stdout == "midi missing bank: ok\n"


def test_midi_borrowed_samples_gm_routes_reference_gain_and_bounded_memory(
    tmp_path: Path,
    native_midi: Path,
) -> None:
    executable = tmp_path / "midi-synth-semantics"
    bank = native_midi / "bank/TimGM6mb.sf2"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DFPLINUX_MIDI_SYNTH_SEMANTICS",
            f'-DFPLINUX_MIDI_SOUNDFONT="{bank}"',
            "-isystem",
            str(native_midi / "include"),
            str(Path(__file__).with_name("midi-render.c")),
            "-lm",
        ],
        name="compile the actual bank renderer's semantic and allocation vectors",
    )
    result = run_process(
        [str(executable)],
        name="check borrowed bank and literal GM behavior",
        timeout=10,
        check=True,
    )
    metrics = json.loads(result.stdout)
    assert metrics["ready_bytes"] < 524288
    assert metrics["copy_pool_bytes"] < 2097152
    assert metrics["peak_bytes"] < 2097152
