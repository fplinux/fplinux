# SPDX-License-Identifier: GPL-2.0-only
"""Host XKB adapter checks with a small fixture map and with real layout data."""

from __future__ import annotations

import shutil
import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

TERMINAL = ROOT / "alpine/aports/fplinux-terminal"
# The build image's xkeyboard-config, which libxkbcommon depends on.
SYSTEM_XKB = Path("/usr/share/X11/xkb")


class TerminalKeyboardTests:
    """Exercise keyboard text and modifiers through the linked XKB library."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            directory = build_directory.name
            cls.executable = Path(directory) / "terminal-keyboard"
            run_process(
                [
                    "cc",
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
                    "-o",
                    str(cls.executable),
                ],
                name="compile terminal XKB harness",
                timeout=30,
                check=True,
            )
            yield

    def test_device_modifiers_symbols_repeat_and_reset(self) -> None:
        """Real XKB state remains consistent across keyboard and phone sources."""
        run_process(
            [str(self.executable), str(ROOT / "tests/host_tool/terminal/fixtures/terminal-xkb")],
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

    data_root: Path
    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            directory = build_directory.name
            cls.data_root = Path(directory) / "xkb"
            for component in ("keycodes", "types", "compat", "symbols"):
                shutil.copytree(SYSTEM_XKB / component, cls.data_root / component)
            shutil.copyfile(
                ROOT / "alpine/aports/fplinux-xkb/fplinux",
                cls.data_root / "symbols/fplinux",
            )
            cls.executable = Path(directory) / "terminal-layout-keys"
            run_process(
                [
                    "cc",
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
                    "-ltsm",
                    "-lxkbcommon",
                    "-o",
                    str(cls.executable),
                ],
                name="compile terminal layout key harness",
                timeout=30,
                check=True,
            )
            yield

    def test_function_scrollback_and_diagnostic_keys_keep_their_actions(self) -> None:
        """F13-F24 send xterm keys; Shift+PageUp scrolls; Ctrl+Alt+F1 asks for the console."""
        run_process(
            [str(self.executable), str(self.data_root)],
            name="run terminal layout key harness",
            timeout=10,
            check=True,
        )
