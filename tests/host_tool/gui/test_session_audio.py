# SPDX-License-Identifier: GPL-2.0-only
"""Manager policy sent over the real private audio control socket."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_session_audio_resyncs_app_and_clock_owners(tmp_path: Path) -> None:
    executable = tmp_path / "session-audio"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            f"-I{ROOT / 'alpine/aports/fplinux-session'}",
            str(ROOT / "tests/host_tool/gui/session-audio-test.c"),
            str(ROOT / "alpine/aports/fplinux-session/session-audio.c"),
        ],
        name="compile manager audio policy client",
    )
    result = run_process(
        [str(executable), str(tmp_path / "control.sock")],
        name="application and clock ownership across a control reconnect",
        timeout=15,
        check=True,
    )
    assert result.stdout == "session audio: OK\n"
