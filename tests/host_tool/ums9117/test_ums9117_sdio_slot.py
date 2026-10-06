# SPDX-License-Identifier: GPL-2.0-only
"""Host-component characterization of slot ownership and power policy."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests import ROOT
from tests.process import run_process

UBOOT = ROOT / "platforms/ums9117/uboot"
HARNESS = ROOT / "tests/host_tool/ums9117/ums9117-sdio-slot.c"
COMPAT = ROOT / "tests/host_tool/ums9117/ums9117-sdio-compat"


class Ums9117SdioSlotTests(unittest.TestCase):
    """Link production slot logic to fake MMIO/ADI; no device is exercised."""

    def test_slot_ownership_and_power_policy(self) -> None:
        """Preserve VSEL admission, EIC ownership and the power-down bit policy."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "ums9117-sdio-slot"
            run_process(
                [
                    "cc",
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{COMPAT}",
                    f"-I{UBOOT}",
                    str(HARNESS),
                    str(UBOOT / "ums9117-sdio-slot.c"),
                    str(UBOOT / "ums9117-sdio-core.c"),
                    "-o",
                    str(executable),
                ],
                name="compile UMS9117 SDIO slot harness",
                timeout=30,
                check=True,
            )
            result = run_process(
                [str(executable)],
                name="run UMS9117 SDIO slot harness",
                timeout=30,
            )
        self.assertEqual(
            result.returncode,
            0,
            msg=f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )


if __name__ == "__main__":
    unittest.main()
