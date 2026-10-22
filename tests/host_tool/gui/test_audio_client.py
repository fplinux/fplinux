# SPDX-License-Identifier: GPL-2.0-only
"""Shared stream client against real sockets and service-owned memfd rings."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture(scope="module")
def audio_client(tmp_path_factory: pytest.TempPathFactory) -> Path:
    executable = tmp_path_factory.mktemp("audio-client") / "audio-client"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/audio-client-test.c"),
            str(ROOT / "lib/fplinux/fplinux-audio-client.c"),
        ],
        name="compile shared audio client with a real socket peer",
    )
    return executable


@pytest.mark.parametrize(
    "scenario", ["ring", "limits", "disconnect", "error", "malformed", "late-answer"]
)
def test_audio_client_stream_contract(audio_client: Path, tmp_path: Path, scenario: str) -> None:
    result = run_process(
        [str(audio_client), str(tmp_path / "audio.sock"), scenario],
        name=f"audio client {scenario}",
        timeout=15,
        check=True,
    )
    assert result.stdout == "audio client: OK\n"
