# SPDX-License-Identifier: GPL-2.0-only
"""Host component regression for RAM-probe payload preservation."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "platforms/ums9117/bootstrap"
HARNESS = ROOT / "tests/host_tool/ums9117-ram-probe.c"


class RamProbeTests(unittest.TestCase):
    """Exercise the bootstrap probe against real host words and aliases."""

    def test_probe_preserves_payload_words_and_alias_detection(self) -> None:
        """Return the vendor detection result without changing any probe word."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "ums9117-ram-probe"
            run_process(
                [
                    "cc",
                    "-std=c99",
                    "-O2",
                    "-pedantic",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{BOOTSTRAP}",
                    str(HARNESS),
                    "-o",
                    str(executable),
                ],
                name="compile RAM-probe harness",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="check RAM-probe payload and alias preservation",
                timeout=10,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
