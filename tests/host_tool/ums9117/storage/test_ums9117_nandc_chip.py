# SPDX-License-Identifier: GPL-2.0-only
"""Compile the real NAND chip selector and B0 policy against independent physical records."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

DRIVER = ROOT / "platforms/ums9117/linux/overlay/drivers/memory/ums9117"
HARNESS = ROOT / "tests/host_tool/ums9117/storage/ums9117-nandc-chip.c"
COMPAT = ROOT / "tests/fixtures/ums9117-kernel/lcm-compat"


def test_nand_ids_select_complete_geometry_and_chip_specific_feature_bits(
    tmp_path_factory: pytest.TempPathFactory,
) -> None:
    """Both INOI layouts and the existing Nokia layout link to the actual chip policy."""
    with c_build_directory(tmp_path_factory, "ums9117_nandc_chip") as temporary:
        executable = temporary / "ums9117-nandc-chip"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{COMPAT}",
                f"-I{DRIVER}",
                str(HARNESS),
                str(DRIVER / "ums9117-nandc-chip.c"),
                str(DRIVER / "ums9117-nandc-feature.c"),
            ],
            name="compile NAND chip and feature policy harness",
            check=True,
        )
        run_process(
            [str(executable)], name="check NAND geometry and B0 policy", timeout=10, check=True
        )
