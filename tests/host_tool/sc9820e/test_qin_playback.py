# SPDX-License-Identifier: GPL-2.0-only
"""Run the driver's playback helper with literal PCM samples and FIFO observations."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e"


@pytest.fixture(scope="module")
def playback_helper(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Substitute the write portals while executing the production helper."""
    with c_build_directory(tmp_path_factory, "qin-playback") as directory:
        executable = directory / "qin-playback"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(FIXTURES / "playback-compat"),
                "-I",
                str(ROOT / "platforms/shared/linux/overlay/include"),
                "-I",
                str(ROOT / "targets/xiaomi-qin-1s-plus/linux/overlay/sound/drivers/qin1splus"),
                str(FIXTURES / "qin-playback.c"),
            ],
            name="compile the Qin playback helper against host write portals",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario", ["samples", "consume-drain", "ring-wrap", "underrun", "invalid-state"]
)
def test_playback_helper_preserves_frame_and_fifo_contracts(
    playback_helper: Path, scenario: str
) -> None:
    """Check data words, actual consumption, drain, wrap and discontinuity errors."""
    run_process(
        [str(playback_helper), scenario],
        name=f"exercise Qin playback helper: {scenario}",
        timeout=5,
        check=True,
    )
