# SPDX-License-Identifier: GPL-2.0-only
"""Host-component checks of LCM transport against a fake MMIO peripheral."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "platforms/ums9117/kernel"
FIXTURES = ROOT / "tests/host_tool"


class Ums9117LcmHostTests(unittest.TestCase):
    """Link the real transport and internal header; no panel is exercised."""

    def test_busy_write_buffer_blocks_commands_and_pixel_mode(self) -> None:
        """Timeouts prevent writes; completing RAMWR admits pixel packing."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "ums9117-lcm"
            compilation = run_process(
                [
                    "cc",
                    "-O2",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-ffunction-sections",
                    "-fdata-sections",
                    f"-I{FIXTURES / 'lcm-compat'}",
                    f"-I{FIXTURES / 'bluetooth-compat'}",
                    f"-I{KERNEL}",
                    str(FIXTURES / "ums9117-lcm.c"),
                    str(KERNEL / "ums9117-fb-lcm.c"),
                    "-Wl,--gc-sections",
                    "-o",
                    str(executable),
                ],
                name="compile UMS9117 LCM host component",
                timeout=30,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stderr)
            result = run_process(
                [str(executable)],
                name="run UMS9117 LCM host component",
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
