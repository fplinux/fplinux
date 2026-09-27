# SPDX-License-Identifier: GPL-2.0-only
"""Input-session component checks with fake evdev, udev and libinput boundaries.

These checks preserve source and device identity around queued lifecycle events.
They do not create a kernel input device or prove libinput's event generation.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]


class InputSessionTests(unittest.TestCase):
    """Observe the production session's caller-visible event sequence."""

    def test_sources_survive_hotplug_and_suspend(self) -> None:
        """Equal keycodes stay source-specific, and reconnect creates a new identity."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "input-session"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'tests/host_tool/input-session-compat'}",
                    f"-I{ROOT / 'include/fplinux'}",
                    str(ROOT / "lib/fplinux/fplinux-input-session.c"),
                    str(ROOT / "tests/host_tool/fplinux-input-session.c"),
                    "-Wl,--wrap=open,--wrap=close,--wrap=ioctl",
                    "-o",
                    str(executable),
                ],
                name="compile input session with fake device boundaries",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="run input source and device lifecycle scenarios",
                timeout=5,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
