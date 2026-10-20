# SPDX-License-Identifier: GPL-2.0-only
"""Host main-loop checks with real rendering and deterministic device doubles."""

from __future__ import annotations

import os
import shlex
import struct
import tempfile
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


@dataclass(frozen=True)
class TerminalLoopHarness:
    """Linked main loop and its immutable fixture font."""

    executable: Path
    font: Path
    child: Path


class TerminalLoopTests:
    """Observe frames and shell input through the host terminal main loop."""

    @pytest.mark.parametrize("scenario", ["echo", "no-echo"], ids=["echo", "no-echo"])
    def test_delayed_echo_and_silent_application_frames(
        self, harness: TerminalLoopHarness, scenario: str
    ) -> None:
        """Commit stays visible until echo; silent input clears without local echo."""
        self._check_scenario(harness, scenario)

    @pytest.mark.parametrize(
        "scenario", ["delayed-hold", "delayed-tap"], ids=["delayed-hold", "delayed-tap"]
    )
    def test_queued_softkey_events_preserve_hold_and_tap(
        self, harness: TerminalLoopHarness, scenario: str
    ) -> None:
        """A queued hold opens Modifiers; a queued tap still needs menu selection."""
        self._check_scenario(harness, scenario)

    @pytest.mark.parametrize(
        ("scenario", "status"),
        [("pty-success", 0), ("pty-failure", 37), ("pty-buffered-failure", 37)],
        ids=["successful-child", "failed-child", "buffered-output"],
    )
    def test_pty_stream_closure_preserves_child_exit_status(
        self, harness: TerminalLoopHarness, tmp_path: Path, scenario: str, status: int
    ) -> None:
        """After PTY closure the last output remains visible and VT dispatch releases the child."""
        result = run_process(
            [
                str(harness.executable),
                str(harness.font),
                str(ROOT / "tests/host_tool/terminal/fixtures/terminal-xkb"),
                scenario,
                str(harness.child),
            ],
            name=f"run terminal loop {scenario} with real PTY",
            timeout=5,
            cwd=tmp_path,
            env={**os.environ, "HOME": str(tmp_path), "LC_ALL": "C.UTF-8"},
            check=False,
        )

        assert result.returncode == status, result.stderr

    @pytest.fixture(scope="class")
    @classmethod
    def harness(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[TerminalLoopHarness]:
        """Link one main-loop harness and its immutable fixture font."""
        with c_build_directory(tmp_path_factory, "terminal-loop") as temporary:
            executable = temporary / "terminal-loop"
            main_object = temporary / "terminal-main.o"
            font = temporary / "fixture.psf"
            child = temporary / "terminal-pty-child"
            header = struct.pack("<8I", 0x864AB572, 0, 32, 1, 2, 12, 12, 8)
            font.write_bytes(header + b"\x00" * 12 + b"\x80" * 12 + b" \xffa\xff")
            flags = run_process(
                ["pkg-config", "--cflags", "libdrm"],
                name="read DRM header flags",
                timeout=10,
                check=True,
            )
            compiler = [
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
            compile_c(
                main_object,
                [
                    *compiler,
                    "-Dmain=fplinux_terminal_main",
                    "-Dpoll=fixture_poll",
                    "-Dclock_gettime=fixture_clock_gettime",
                    "-c",
                    str(TERMINAL / "fplinux-terminal.c"),
                ],
                name="compile terminal main loop",
                check=True,
            )
            compile_c(
                child,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(ROOT / "tests/host_tool/terminal/fplinux-terminal-pty-child.c"),
                ],
                name="compile terminal PTY child",
                check=True,
            )
            compile_c(
                executable,
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
                    str(TERMINAL / "terminal-pty.c"),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                    str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                    str(ROOT / "lib/fplinux/fplinux-language.c"),
                    str(ROOT / "lib/fplinux/fplinux-keyboard-text.c"),
                    "-Wl,--wrap=fplinux_terminal_keyboard_open",
                    "-Wl,--wrap=fplinux_terminal_pty_open",
                    "-Wl,--wrap=fplinux_terminal_pty_shell_foreground",
                    "-Wl,--wrap=fplinux_terminal_pty_finished",
                    "-Wl,--wrap=fplinux_terminal_pty_close",
                    "-ltsm",
                    "-lxkbcommon",
                ],
                name="link terminal loop harness",
                check=True,
            )
            yield TerminalLoopHarness(executable, font, child)

    def _check_scenario(self, harness: TerminalLoopHarness, scenario: str) -> None:
        """Run one process with fresh shell state and owned files."""
        with tempfile.TemporaryDirectory() as directory:
            result = run_process(
                [
                    str(harness.executable),
                    str(harness.font),
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
