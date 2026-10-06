# SPDX-License-Identifier: GPL-2.0-only
"""Host checks with pipe-backed evdev substitutes and stubbed VT ioctls."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests import ROOT
from tests.process import run_process


class TerminalDiagnosticTests(unittest.TestCase):
    """Exercise the diagnostic return owner without a phone or a live VT."""

    executable: Path

    @classmethod
    def setUpClass(cls) -> None:
        """Link the production owner; retain real descriptor and stream operations."""
        temporary = tempfile.TemporaryDirectory(prefix="fplinux-terminal-diagnostic-")
        cls.addClassCleanup(temporary.cleanup)
        cls.executable = Path(temporary.name) / "terminal-diagnostic"
        run_process(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{ROOT / 'alpine/aports/fplinux-terminal'}",
                str(ROOT / "tests/host_tool/terminal/fplinux-terminal-diagnostic.c"),
                str(ROOT / "alpine/aports/fplinux-terminal/terminal-diagnostic.c"),
                "-Wl,--wrap=open,--wrap=ioctl",
                "-o",
                str(cls.executable),
            ],
            name="compile terminal diagnostic host harness",
            timeout=30,
            check=True,
        )

    def run_scenario(self, scenario: str) -> None:
        """Run one bounded scenario with independent descriptor state."""
        run_process(
            [str(self.executable), scenario],
            name=f"exercise terminal diagnostic {scenario}",
            timeout=10,
            check=True,
        )

    def test_right_soft_tap_returns_to_allocated_terminal_vt(self) -> None:
        """A complete tap survives input suspension and returns after key release."""
        self.run_scenario("return")

    def test_each_phone_keypad_can_return_and_external_f14_is_untouched(self) -> None:
        """The action belongs to phone input sources and keeps device press state."""
        self.run_scenario("sources")

    def test_other_vt_does_not_switch_and_closed_watch_leaves_events_unread(self) -> None:
        """Other VTs are not switched; closing the watch leaves reused descriptors alone."""
        self.run_scenario("ownership")


if __name__ == "__main__":
    unittest.main()
