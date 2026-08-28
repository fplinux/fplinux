# SPDX-License-Identifier: GPL-2.0-only
"""Host component test for the shared C11 numeric-keypad multi-tap core."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "alpine/shared/fplinux-multitap.c"
HARNESS = ROOT / "tests/host_tool/fplinux-multitap-core.c"
INCLUDE = ROOT / "alpine/shared"


class MultiTapCoreTests(unittest.TestCase):
    """Exercise the portable state machine without target hardware."""

    def test_c11_core_contract(self) -> None:
        """Exact groups and time boundaries remain shared behavior."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "fplinux-multitap-core"
            run_process(
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
                name="compile multi-tap harness",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="run multi-tap harness",
                timeout=10,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
