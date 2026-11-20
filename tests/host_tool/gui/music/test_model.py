# SPDX-License-Identifier: GPL-2.0-only
"""User transport decisions with literal previous, queue and repeat cases."""

from pathlib import Path

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process


def test_previous_threshold_manual_wrap_and_natural_repeat(tmp_path: Path) -> None:
    executable = tmp_path / "music-model"
    aport = ROOT / "alpine/aports/fplinux-music"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{aport}",
            str(Path(__file__).parent / "native/music-model.c"),
            str(aport / "music-model.c"),
        ],
        name="compile the music transport decision model",
    )
    result = run_process(
        [str(executable)], name="check previous, next and repeat decisions", timeout=10, check=True
    )
    assert result.stdout == "music model: ok\n"
