# SPDX-License-Identifier: GPL-2.0-only
"""Host-tool checks for the production ARMADA scene and timeline."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
APORT = ROOT / "alpine/aports/fplinux-showcase"
HARNESS = ROOT / "tests/host_tool/fplinux-showcase.c"
SCENE = APORT / "armada-scene.c"


class FplinuxShowcaseHostToolTests(unittest.TestCase):
    """Render real frames without claiming framebuffer or phone coverage."""

    def test_renderer_and_timeline_at_both_display_sizes(self) -> None:
        """Protect bounds, frame-history independence, cues, and loop wrapping."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "fplinux-showcase-test"
            run_process(
                [
                    "cc",
                    "-O2",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{APORT}",
                    str(HARNESS),
                    str(SCENE),
                    "-o",
                    str(executable),
                ],
                name="compile FPLinux showcase host harness",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="run FPLinux showcase host harness",
                timeout=30,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
