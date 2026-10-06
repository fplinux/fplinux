# SPDX-License-Identifier: GPL-2.0-only
"""Host checks with pipe-backed evdev substitutes and stubbed VT ioctls."""

from __future__ import annotations

import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


class TerminalDiagnosticTests:
    """Exercise the diagnostic return owner without a phone or a live VT."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the production owner; retain real descriptor and stream operations."""
        with ExitStack() as cleanup:
            temporary = tempfile.TemporaryDirectory(prefix="fplinux-terminal-diagnostic-")
            cleanup.enter_context(temporary)
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
            yield

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
