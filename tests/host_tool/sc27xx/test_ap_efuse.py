# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the registered AP efuse provider with host lock, clock and MMIO boundaries."""

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

FIXTURES = ROOT / "tests/host_tool/sc27xx/efuse"


@pytest.fixture(scope="module")
def ap_efuse(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Apply the production patch and link the whole provider as a separate translation unit."""
    with c_build_directory(tmp_path_factory, "sprd-ap-efuse") as directory:
        driver = directory / "drivers/nvmem/sprd-efuse.c"
        driver.parent.mkdir(parents=True)
        shutil.copyfile(FIXTURES / "sprd-efuse.c.fixture", driver)
        environment = os.environ.copy()
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR"):
            environment.pop(name, None)
        environment["GIT_CEILING_DIRECTORIES"] = str(directory)
        run_process(
            [
                "git",
                "apply",
                "--include=drivers/nvmem/sprd-efuse.c",
                str(ROOT / "platforms/shared/linux/patches/0015-sprd-ap-efuse.patch"),
            ],
            cwd=directory,
            env=environment,
            name="apply the shared AP efuse patch to its upstream fixture",
            timeout=15,
            check=True,
        )
        executable = directory / "ap-efuse"
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
                str(FIXTURES / "ap-efuse.c"),
                str(driver),
            ],
            name="link the AP efuse provider with host hardware boundaries",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario",
    ["qin-words", "qin-errors", "ums312", "ums9117-words", "ums9117-errors"],
    ids=[
        "qin-read-only-window-multiword-and-unaligned-bytes",
        "qin-zero-is-success-errors-stop-and-release-resources",
        "ums312-offset-and-programming-registration-are-preserved",
        "ums9117-guarded-read-only-window-and-mode-restoration",
        "ums9117-refuses-inherited-state-and-preserves-new-error-flags",
    ],
)
def test_ap_efuse_provider_reads_and_errors(ap_efuse: Path, scenario: str) -> None:
    """Host register substitutes do not establish a physical efuse read."""
    run_process(
        [str(ap_efuse), scenario],
        name=f"exercise the registered AP efuse provider: {scenario}",
        timeout=5,
        check=True,
    )
