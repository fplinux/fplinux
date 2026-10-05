# SPDX-License-Identifier: GPL-2.0-only
"""Host C checks of defined B0 feature bits and readback comparisons."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "platforms/ums9117/linux/arch/arm/mach-ums9117"
HARNESS = ROOT / "tests/host_tool/ums9117-nandc-feature.c"
COMPAT = ROOT / "tests/host_tool/lcm-compat"


class Ums9117NandcFeatureTests(unittest.TestCase):
    """Link the feature policy with host type aliases; no NAND is exercised."""

    def test_feature_values_preserve_defined_bits_and_detect_changed_state(self) -> None:
        """Accept reserved-bit drift while detecting ECC and other state changes."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "ums9117-nandc-feature"
            run_process(
                [
                    "cc",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{COMPAT}",
                    f"-I{DRIVER}",
                    str(HARNESS),
                    str(DRIVER / "ums9117-nandc-feature.c"),
                    "-o",
                    str(executable),
                ],
                name="compile UMS9117 NANDC feature policy harness",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="check UMS9117 NANDC feature values and state comparisons",
                timeout=10,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
