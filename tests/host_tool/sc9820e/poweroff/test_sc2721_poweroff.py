# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real exported atomic poweroff helper with host hardware boundaries."""

from __future__ import annotations

import os
import shutil
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/poweroff"


@pytest.fixture(scope="module")
def atomic_poweroff(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Apply both real ADI patches and link the complete driver separately."""
    with c_build_directory(tmp_path_factory, "sc2721-poweroff") as directory:
        spi = directory / "drivers/spi"
        spi.mkdir(parents=True)
        shutil.copyfile(FIXTURES / "spi-sprd-adi.c.fixture", spi / "spi-sprd-adi.c")
        patches = [
            ROOT / "platforms/shared/linux/patches/0002-sprd-adi.patch",
            ROOT / "platforms/sc9820e/linux/patches/0014-sc2721-poweroff.patch",
        ]
        environment = os.environ.copy()
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR"):
            environment.pop(name, None)
        environment["GIT_CEILING_DIRECTORIES"] = str(directory)
        for patch in patches:
            run_process(
                ["git", "apply", "--include=drivers/spi/spi-sprd-adi.c", str(patch)],
                cwd=directory,
                env=environment,
                name=f"apply the ADI driver changes from {patch.name}",
                timeout=15,
                check=True,
            )
        executable = directory / "sc2721-poweroff"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-I",
                str(FIXTURES / "compat"),
                "-I",
                str(ROOT / "platforms/shared/linux/overlay/include"),
                str(FIXTURES / "atomic-poweroff.c"),
                str(spi / "spi-sprd-adi.c"),
            ],
            name="link the ADI probe and atomic poweroff helper with host hardware boundaries",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario",
    ["success", "identity", "lock", "unlock-timeout", "unlock-error", "final-fifo", "spi"],
    ids=[
        "acknowledged-poweroff-preserves-unrelated-registers",
        "identity-and-context-failures-cause-no-power-writes",
        "atomic-lock-failure-causes-no-power-writes",
        "unlock-timeout-never-requests-powerdown",
        "unlock-readback-error-never-requests-powerdown",
        "final-fifo-error-is-reported-without-retry",
        "ordinary-spi-values-and-readback-errors-are-preserved",
    ],
)
def test_sc2721_atomic_poweroff(atomic_poweroff: Path, scenario: str) -> None:
    """Host MMIO/semaphore substitutes cannot establish electrical power removal."""
    run_process(
        [str(atomic_poweroff), scenario],
        name=f"exercise the real SC2721 atomic transaction: {scenario}",
        timeout=5,
        check=True,
    )
