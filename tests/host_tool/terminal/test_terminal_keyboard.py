# SPDX-License-Identifier: GPL-2.0-only
"""Host XKB adapter checks with a small fixture map and with real layout data."""

from __future__ import annotations

import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

TERMINAL = ROOT / "alpine/aports/fplinux-terminal"
# The build image's xkeyboard-config, which libxkbcommon depends on.
SYSTEM_XKB = Path("/usr/share/X11/xkb")


@dataclass(frozen=True)
class LayoutKeyboardHarness:
    """Linked keyboard engine and its immutable copied layout data."""

    executable: Path
    data_root: Path


class TerminalKeyboardTests:
    """Exercise keyboard text and modifiers through the linked XKB library."""

    @pytest.fixture(scope="class")
    @classmethod
    def executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "terminal-keyboard") as directory:
            executable = directory / "terminal-keyboard"
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include/fplinux"),
                    "-I",
                    str(TERMINAL),
                    str(ROOT / "tests/host_tool/terminal/fplinux-terminal-keyboard.c"),
                    str(TERMINAL / "terminal-keyboard.c"),
                    str(ROOT / "lib/fplinux/fplinux-keyboard-text.c"),
                    "-lxkbcommon",
                ],
                name="compile terminal XKB harness",
                check=True,
            )
            yield executable

    def test_device_modifiers_symbols_repeat_and_reset(self, executable: Path) -> None:
        """Real XKB state remains consistent across keyboard and phone sources."""
        run_process(
            [str(executable), str(ROOT / "tests/host_tool/terminal/fixtures/terminal-xkb")],
            name="run terminal XKB harness",
            timeout=10,
            check=True,
        )


class TerminalLayoutKeyTests:
    """Run documented keyboard keys through the terminal with real layout data.

    The data root holds the upstream xkeyboard-config files installed in the
    build image and the tracked ``fplinux(function_keys)`` symbols. It is not
    the built ``fplinux-xkb`` package, and nothing here runs on a phone.
    """

    @pytest.fixture(scope="class")
    @classmethod
    def harness(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[LayoutKeyboardHarness]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "terminal-layout-keys") as directory:
            data_root = directory / "xkb"
            for component in ("keycodes", "types", "compat", "symbols"):
                shutil.copytree(SYSTEM_XKB / component, data_root / component)
            shutil.copyfile(
                ROOT / "alpine/aports/fplinux-xkb/fplinux",
                data_root / "symbols/fplinux",
            )
            executable = directory / "terminal-layout-keys"
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include/fplinux"),
                    "-I",
                    str(TERMINAL),
                    str(ROOT / "tests/host_tool/terminal/fplinux-terminal-shipped-keyboard.c"),
                    str(TERMINAL / "terminal-engine.c"),
                    str(TERMINAL / "terminal-input.c"),
                    str(TERMINAL / "terminal-help.c"),
                    str(TERMINAL / "terminal-keyboard.c"),
                    str(ROOT / "lib/fplinux/fplinux-keyboard-text.c"),
                    str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                    str(ROOT / "lib/fplinux/fplinux-language.c"),
                    "-ltsm",
                    "-lxkbcommon",
                ],
                name="compile terminal layout key harness",
                check=True,
            )
            yield LayoutKeyboardHarness(executable, data_root)

    def test_function_scrollback_and_diagnostic_keys_keep_their_actions(
        self, harness: LayoutKeyboardHarness
    ) -> None:
        """F13-F24 send xterm keys; Shift+PageUp scrolls; Ctrl+Alt+F1 asks for the console."""
        run_process(
            [str(harness.executable), str(harness.data_root)],
            name="run terminal layout key harness",
            timeout=10,
            check=True,
        )
