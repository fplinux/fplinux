# SPDX-License-Identifier: GPL-2.0-only
"""Host-component tests for the shared UMS9117 SDIO controller core."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMMON = ROOT / "platforms/ums9117/common"
HARNESS = ROOT / "tests/ums9117-sdio-core.c"
COMPAT = ROOT / "tests/ums9117-sdio-compat"


class Ums9117SdioCoreTests(unittest.TestCase):
    """Link the production core to a fake MMIO/timer hardware boundary."""

    def test_controller_sequences_and_fail_closed_boundaries(self) -> None:
        """Preserve proven writes and stop after named reset/status failures."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "ums9117-sdio-core"
            subprocess.run(
                [
                    "cc",
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{COMPAT}",
                    f"-I{COMMON}",
                    str(HARNESS),
                    str(COMMON / "ums9117-sdio-core.c"),
                    "-o",
                    str(executable),
                ],
                check=True,
                timeout=30,
            )
            result = subprocess.run(
                [str(executable)],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
        self.assertEqual(
            result.returncode,
            0,
            msg=f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )


if __name__ == "__main__":
    unittest.main()
