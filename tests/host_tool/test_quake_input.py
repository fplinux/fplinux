# SPDX-License-Identifier: GPL-2.0-only
"""Native Quake input mapping with fake engine, display and input-session boundaries."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]


class QuakeInputTests(unittest.TestCase):
    """Observe engine key states without running the game, DRM or libinput."""

    def test_sources_and_shared_actions_survive_releases_and_focus_reset(self) -> None:
        """Releasing one key cannot stop an action still held by another key."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "quake-input"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'tests/host_tool/quake-input-compat'}",
                    f"-I{ROOT / 'include/fplinux'}",
                    f"-I{ROOT / 'alpine/aports/fplinux-tyrquake'}",
                    str(ROOT / "alpine/aports/fplinux-tyrquake/in_fplinux.c"),
                    str(ROOT / "tests/host_tool/fplinux-quake-input.c"),
                    "-o",
                    str(executable),
                ],
                name="compile Quake input with fake runtime boundaries",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="observe Quake key mapping and held actions",
                timeout=5,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
