# SPDX-License-Identifier: GPL-2.0-only
"""Link PCM callbacks and FIFO servicing to a bounded host ALSA RESET model."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

AUDIO = ROOT / "platforms/ums9117/linux/overlay/sound/drivers/ums9117"
FIXTURES = ROOT / "tests/host_tool/ums9117/audio"
COMPAT = FIXTURES / "pcm-reset-compat"
EXISTING_COMPAT = FIXTURES / "native-operation-compat/audio"


@pytest.fixture(scope="module")
def pcm_reset(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the complete PCM driver once for both callback scenarios."""
    with c_build_directory(tmp_path_factory, "pcm-reset") as temporary:
        executable = temporary / "pcm-reset"
        compile_c(
            executable,
            [
                "-O2",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-Wno-sign-compare",
                "-ffunction-sections",
                "-fdata-sections",
                f"-I{COMPAT}",
                f"-I{EXISTING_COMPAT}",
                f"-I{AUDIO}",
                f"-I{ROOT / 'platforms/shared/linux/overlay/include'}",
                str(FIXTURES / "pcm-reset.c"),
                str(AUDIO / "ums9117-pcm.c"),
                "-Wl,--gc-sections",
            ],
            name="link PCM callbacks with host ALSA and FIFO boundaries",
            check=True,
        )
        yield executable


def test_running_capture_reset_rebases_fifo_without_interrupting_playback(pcm_reset: Path) -> None:
    """Capture wraps, resets with a timer interleave, then records more samples."""
    result = run_process(
        [str(pcm_reset), "capture-wrap"],
        name="exercise running capture RESET with concurrent playback",
        timeout=10,
        check=True,
    )
    assert result.stdout == "PCM RESET callback checks passed\n"


def test_capture_reset_before_pending_notification_preserves_running_stream(
    pcm_reset: Path,
) -> None:
    """Deliver a real pending notification before the first capture ring wrap."""
    result = run_process(
        [str(pcm_reset), "pending-notification"],
        name="exercise pending PCM period notification after running RESET",
        timeout=10,
        check=True,
    )
    assert result.stdout == "PCM RESET callback checks passed\n"
