# SPDX-License-Identifier: GPL-2.0-only
"""Host-component characterization of slot ownership and power policy."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / "platforms/ums9117/common"
UBOOT = ROOT / "platforms/ums9117/uboot"
HARNESS = ROOT / "tests/host_tool/ums9117-sdio-slot.c"
COMPAT = ROOT / "tests/host_tool/ums9117-sdio-compat"


class Ums9117SdioSlotTests(unittest.TestCase):
    """Link production slot logic to fake MMIO/ADI; no device is exercised."""

    def test_slot_ownership_and_power_policy(self) -> None:
        """Preserve VSEL admission, EIC ownership and the power-down bit policy."""
        for target, board in (
            ("nokia-ta1618", "ta1618"),
            ("inoi-244-modern-4g", "inoi244"),
        ):
            with self.subTest(target=target):
                self.check_slot(target, board)

    def check_slot(self, target: str, board: str) -> None:
        """Link the selected board and verify its observable slot operations."""
        target_root = ROOT / "targets" / target
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
                    f"-I{COMMON}",
                    f"-I{UBOOT}",
                    f"-I{target_root / 'common'}",
                    str(HARNESS),
                    str(target_root / "common" / f"{board}-sdio-board.c"),
                    str(target_root / "uboot/slot.c"),
                    str(COMMON / "ums9117-sdio-slot.c"),
                    str(COMMON / "ums9117-sdio-core.c"),
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
