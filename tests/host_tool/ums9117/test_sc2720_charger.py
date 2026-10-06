# SPDX-License-Identifier: GPL-2.0-only
"""Host charger callbacks with fake regmap and power-supply registration."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests import ROOT
from tests.process import run_process

FIXTURES = ROOT / "tests/host_tool/ums9117"
DRIVER = ROOT / "platforms/ums9117/linux/drivers/power/supply/sc2720-charger-ums9117.c"


class Sc2720ChargerHostTests(unittest.TestCase):
    """Link the real driver; no kernel or physical charger is exercised."""

    def test_usb_type_online_and_read_only_errors(self) -> None:
        """Decode status snapshots without writes and preserve callback errors."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "sc2720-charger"
            compilation = run_process(
                [
                    "cc",
                    "-O2",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{FIXTURES / 'charger-compat'}",
                    str(FIXTURES / "sc2720-charger.c"),
                    str(DRIVER),
                    "-o",
                    str(executable),
                ],
                name="compile SC2720 charger host component",
                timeout=30,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stderr)
            result = run_process(
                [str(executable)],
                name="run SC2720 charger host component",
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
