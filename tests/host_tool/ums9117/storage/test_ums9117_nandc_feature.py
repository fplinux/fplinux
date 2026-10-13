# SPDX-License-Identifier: GPL-2.0-only
"""Host C checks of defined B0 feature bits and readback comparisons."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

DRIVER = ROOT / "platforms/ums9117/linux/overlay/drivers/memory/ums9117"
HARNESS = ROOT / "tests/host_tool/ums9117/storage/ums9117-nandc-feature.c"
COMPAT = ROOT / "tests/fixtures/ums9117-kernel/lcm-compat"


class Ums9117NandcFeatureTests:
    """Link the feature policy with host type aliases; no NAND is exercised."""

    def test_feature_values_preserve_defined_bits_and_detect_changed_state(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Accept reserved-bit drift while detecting ECC and other state changes."""
        with c_build_directory(tmp_path_factory, "ums9117_nandc_feature") as temporary:
            executable = temporary / "ums9117-nandc-feature"
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
                    str(DRIVER / "ums9117-nandc-feature.c"),
                ],
                name="compile UMS9117 NANDC feature policy harness",
                check=True,
            )
            run_process(
                [str(executable)],
                name="check UMS9117 NANDC feature values and state comparisons",
                timeout=10,
                check=True,
            )
