# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for FM error recovery through the shared radio owner."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/overlay/drivers/mmc/host/bq2830l-radio"
FIXTURES = ROOT / "tests/host_tool/bq_2830l_tank_connect_max/radio"
KERNEL_COMPAT = ROOT / "tests/fixtures/ums9117-kernel"
PLATFORM = ROOT / "platforms/ums9117/linux/overlay"


@pytest.fixture(scope="module")
def fm_runtime(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link FM callbacks and transport; V4L2, board, IRQ and SDIO are explicit doubles."""
    with c_build_directory(tmp_path_factory, "marlin2-fm-recovery") as temporary:
        executable = temporary / "marlin2-fm-recovery"
        compile_c(
            executable,
            [
                "-O2",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-UNDEBUG",
                "-pthread",
                f"-I{FIXTURES}",
                f"-I{FIXTURES / 'marlin2-owner-compat'}",
                f"-I{KERNEL_COMPAT / 'fm-compat'}",
                f"-I{KERNEL_COMPAT / 'bluetooth-compat'}",
                f"-I{KERNEL}",
                f"-I{PLATFORM / 'include'}",
                str(FIXTURES / "marlin2-fm.c"),
                str(KERNEL / "marlin2-fm.c"),
                str(PLATFORM / "lib/ums9117-fm-wire.c"),
                str(KERNEL / "marlin2-transport.c"),
                str(KERNEL / "marlin2-transport-wire.c"),
                str(KERNEL / "marlin2-packet-codec.c"),
            ],
            name="compile Marlin2 FM recovery host consumer checks",
            check=True,
        )
        yield executable


def test_failed_seek_recovers_on_fresh_runtime_and_preserves_other_consumers(
    fm_runtime: Path,
) -> None:
    result = run_process(
        [str(fm_runtime)],
        name="run Marlin2 FM recovery host consumer checks",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("Marlin2 FM fresh runtime host consumer cases passed\n")


@pytest.mark.parametrize("scenario", ["mute-reopen", "mute-recovery"])
def test_unmute_after_reopen_sends_command(fm_runtime: Path, scenario: str) -> None:
    """The V4L2 fake skips unchanged controls across ordinary and recovered opens."""
    result = run_process(
        [str(fm_runtime), scenario],
        name="run Marlin2 FM reopen mute host consumer check",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Marlin2 FM reopen mute host consumer case passed\n"
