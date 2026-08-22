# SPDX-License-Identifier: GPL-2.0-only
"""Host component test for the shared C11 numeric-keypad multi-tap core."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "alpine/shared/fplinux-multitap.c"
HARNESS = ROOT / "tests/fplinux-multitap-core.c"
INCLUDE = ROOT / "alpine/shared"


class MultiTapCoreTests(unittest.TestCase):
    """Exercise the portable state machine without target hardware."""

    def test_c11_core_contract(self) -> None:
        """Exact groups and time boundaries remain shared behavior."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "fplinux-multitap-core"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(INCLUDE),
                    str(HARNESS),
                    str(CORE),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
