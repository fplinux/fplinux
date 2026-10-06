# SPDX-License-Identifier: GPL-2.0-only
"""Host main-loop checks with real rendering and deterministic device doubles."""

from __future__ import annotations

import os
import shlex
import struct
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


class TerminalLoopTests:
    """Observe frames and shell input through the host terminal main loop."""

    executable: Path
    font: Path

    @pytest.mark.parametrize("scenario", ["echo", "no-echo"], ids=["echo", "no-echo"])
    def test_delayed_echo_and_silent_application_frames(self, scenario: str) -> None:
        """Commit stays visible until echo; silent input clears without local echo."""
        self._check_scenario(scenario)

    @pytest.mark.parametrize(
        "scenario", ["delayed-hold", "delayed-tap"], ids=["delayed-hold", "delayed-tap"]
    )
    def test_queued_softkey_events_preserve_hold_and_tap(self, scenario: str) -> None:
        """A queued hold opens Modifiers; a queued tap still needs menu selection."""
        self._check_scenario(scenario)

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link one main-loop harness and its immutable fixture font."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            directory = build_directory.name
            temporary = Path(directory)
            cls.executable = temporary / "terminal-loop"
            main_object = temporary / "terminal-main.o"
            cls.font = temporary / "fixture.psf"
            header = struct.pack("<8I", 0x864AB572, 0, 32, 1, 2, 12, 12, 8)
            cls.font.write_bytes(header + b"\x00" * 12 + b"\x80" * 12 + b" \xffa\xff")
            flags = run_process(
                ["pkg-config", "--cflags", "libdrm"],
                name="read DRM header flags",
                timeout=10,
                check=True,
            )
            compiler = [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include/fplinux"),
                "-I",
                str(TERMINAL),
                *shlex.split(flags.stdout),
            ]
            run_process(
                [
                    *compiler,
                    "-Dmain=fplinux_terminal_main",
                    "-Dpoll=fixture_poll",
                    "-Dclock_gettime=fixture_clock_gettime",
                    "-c",
                    str(TERMINAL / "fplinux-terminal.c"),
                    "-o",
                    str(main_object),
                ],
                name="compile terminal main loop",
                timeout=30,
                check=True,
            )
            run_process(
                [
                    *compiler,
                    str(ROOT / "tests/host_tool/terminal/fplinux-terminal-loop.c"),
                    str(main_object),
                    str(TERMINAL / "terminal-engine.c"),
                    str(TERMINAL / "terminal-help.c"),
                    str(TERMINAL / "terminal-input.c"),
                    str(TERMINAL / "terminal-render.c"),
                    str(TERMINAL / "terminal-keyboard.c"),
                    str(TERMINAL / "terminal-diagnostic.c"),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                    str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                    str(ROOT / "lib/fplinux/fplinux-keyboard-text.c"),
                    "-Wl,--wrap=fplinux_terminal_keyboard_open",
                    "-ltsm",
                    "-lxkbcommon",
                    "-o",
                    str(cls.executable),
                ],
                name="link terminal loop harness",
                timeout=30,
                check=True,
            )
            yield

    def _check_scenario(self, scenario: str) -> None:
        """Run one process with fresh shell state and owned files."""
        with tempfile.TemporaryDirectory() as directory:
            result = run_process(
                [
                    str(self.executable),
                    str(self.font),
                    str(ROOT / "tests/host_tool/terminal/fixtures/terminal-xkb"),
                    scenario,
                ],
                name=f"run terminal loop {scenario} harness",
                timeout=10,
                cwd=Path(directory),
                env={
                    **os.environ,
                    "HOME": directory,
                    "HISTFILE": str(Path(directory) / ".bash_history"),
                    "LC_ALL": "C.UTF-8",
                },
                check=False,
            )
            assert (result.returncode) == (0), result.stderr
