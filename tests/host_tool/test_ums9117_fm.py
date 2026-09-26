# SPDX-License-Identifier: GPL-2.0-only
"""FM driver callback checks with synthetic firmware and V4L2 registration."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "platforms/ums9117/linux/drivers/bluetooth/ums9117"
FIXTURES = ROOT / "tests/host_tool"


class Ums9117FMHostTests(unittest.TestCase):
    """Link the production driver; no V4L2 core or hardware is exercised."""

    def test_close_restores_clock_or_retains_quarantined_hold(self) -> None:
        """Callbacks preserve RF bits, validate firmware replies and retain failed holds."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "fm"
            run_process(
                [
                    "cc",
                    "-O2",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-unused-parameter",
                    "-Wno-sign-compare",
                    "-UNDEBUG",
                    "-DCONFIG_RADIO_UMS9117_CM4=1",
                    f"-I{FIXTURES / 'fm-compat'}",
                    f"-I{FIXTURES / 'bluetooth-compat'}",
                    f"-I{KERNEL}",
                    str(FIXTURES / "ums9117-fm.c"),
                    str(KERNEL / "cm4-fm.c"),
                    "-o",
                    str(executable),
                ],
                name="compile UMS9117 FM driver host callbacks",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="run UMS9117 FM driver host callbacks",
                timeout=10,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
