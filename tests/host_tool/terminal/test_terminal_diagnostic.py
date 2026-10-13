# SPDX-License-Identifier: GPL-2.0-only
"""Host checks with pipe-backed evdev substitutes and stubbed VT ioctls."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


class TerminalDiagnosticTests:
    """Exercise the diagnostic return owner without a phone or a live VT."""

    @pytest.fixture(scope="class")
    @classmethod
    def executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
        """Link the production owner; retain real descriptor and stream operations."""
        with c_build_directory(tmp_path_factory, "fplinux-terminal-diagnostic") as directory:
            executable = directory / "terminal-diagnostic"
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'include/fplinux'}",
                    f"-I{ROOT / 'alpine/aports/fplinux-terminal'}",
                    str(ROOT / "tests/host_tool/terminal/fplinux-terminal-diagnostic.c"),
                    str(ROOT / "alpine/aports/fplinux-terminal/terminal-diagnostic.c"),
                    "-Wl,--wrap=open,--wrap=ioctl",
                ],
                name="compile terminal diagnostic host harness",
                check=True,
            )
            yield executable

    def run_scenario(self, executable: Path, scenario: str) -> None:
        """Run one bounded scenario with independent descriptor state."""
        run_process(
            [str(executable), scenario],
            name=f"exercise terminal diagnostic {scenario}",
            timeout=10,
            check=True,
        )

    def test_right_soft_tap_returns_to_allocated_terminal_vt(self, executable: Path) -> None:
        """A complete tap survives input suspension and returns after key release."""
        self.run_scenario(executable, "return")

    def test_each_phone_keypad_can_return_and_external_f14_is_untouched(
        self, executable: Path
    ) -> None:
        """The action belongs to phone input sources and keeps device press state."""
        self.run_scenario(executable, "sources")

    def test_other_vt_does_not_switch_and_closed_watch_leaves_events_unread(
        self, executable: Path
    ) -> None:
        """Other VTs are not switched; closing the watch leaves reused descriptors alone."""
        self.run_scenario(executable, "ownership")
