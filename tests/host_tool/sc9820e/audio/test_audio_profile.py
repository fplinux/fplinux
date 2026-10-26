# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked Qin audio profile, MMIO admission and restoration behavior."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest


def test_prepared_profile_and_processing_preserve_the_runtime_contract(
    tmp_path_factory: pytest.TempPathFactory,
) -> None:
    """Replace firmware and MMIO while linking the real parser and processing owner."""
    source = ROOT / "targets/xiaomi-qin-1s-plus/linux/overlay/sound/drivers/qin1splus"
    fixture = ROOT / "tests/host_tool/sc9820e/audio"
    with c_build_directory(tmp_path_factory, "qin_audio") as directory:
        executable = directory / "qin-audio"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D__iomem=",
                f"-I{fixture / 'compat'}",
                f"-I{ROOT / 'tests/host_tool/ums9117/audio/native-operation-compat/audio'}",
                f"-I{ROOT / 'tests/fixtures/ums9117-kernel/fm-compat'}",
                f"-I{ROOT / 'platforms/shared/linux/overlay/include'}",
                f"-I{source}",
                str(fixture / "audio-profile.c"),
                str(source / "qin-audio-profile.c"),
                str(source / "qin-audio-processing.c"),
            ],
            name="compile Qin audio production components",
        )
        run_process(
            [str(executable)],
            name="exercise Qin audio profile and processing",
            timeout=30,
            check=True,
        )
