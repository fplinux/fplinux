# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for the production audio mixing core."""

from pathlib import Path

from tests.c_harness import compile_c
from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]
APORT = ROOT / "alpine/aports/fplinux-audiod"


def test_mixer_resamples_ramps_saturates_and_tracks_heard(tmp_path: Path) -> None:
    executable = tmp_path / "audio-mixer-test"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(APORT),
            str(Path(__file__).with_name("audio-mixer-test.c")),
            str(APORT / "audio-mixer.c"),
            "-lm",
        ],
        name="compile audio mixer host harness",
    )
    result = run_process(
        [str(executable)], name="run audio mixer host harness", timeout=10, check=True
    )
    assert result.stdout == "audio mixer: ok\n"
