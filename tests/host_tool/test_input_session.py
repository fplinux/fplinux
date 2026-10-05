# SPDX-License-Identifier: GPL-2.0-only
"""Input-session components with controlled udev, ioctl and libevdev boundaries.

Real epoll, pipes and eventfd exercise readiness. Library synchronization is
stubbed; these checks do not create a kernel input device or prove kernel loss
recovery, hotplug or grabs.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]


class InputSessionTests(unittest.TestCase):
    """Observe the production session's caller-visible event sequence."""

    def test_event_translation_and_device_lifetimes(self) -> None:
        """Literal traces preserve identity, frames, repeat and synchronized state."""
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
                    "-Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=nanosleep",
                    "-o",
                    str(executable),
                ],
                name="compile input session with fake device boundaries",
                timeout=30,
                check=True,
            )
            for scenario in (
                "lifecycle",
                "classification",
                "rejected",
                "modifiers",
                "frames",
                "pointer",
                "sync",
                "retry",
            ):
                with self.subTest(scenario=scenario):
                    run_process(
                        [str(executable), scenario],
                        name=f"run input-session {scenario} component scenario",
                        timeout=5,
                        check=True,
                    )


if __name__ == "__main__":
    unittest.main()
