# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of the linked libtsm engine, a memory surface and a real Bash PTY."""

from __future__ import annotations

import os
import struct
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.alpine.aports import materialize_aport_sources

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


@dataclass(frozen=True)
class TerminalEngineHarness:
    """Linked terminal engine with its shell startup and immutable fixture fonts."""

    executable: Path
    bashrc: Path
    fonts: tuple[Path, ...]


class TerminalEngineTests:
    """Run each terminal scenario in its own harness process."""

    @pytest.fixture(scope="class")
    @classmethod
    def harness(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[TerminalEngineHarness]:
        """Link the staged terminal sources and write the fixture fonts."""
        with c_build_directory(tmp_path_factory, "fplinux-terminal-engine") as directory:
            aport = materialize_aport_sources("fplinux-terminal", ROOT, directory / "aport")
            executable = directory / "terminal-engine"
            fonts = []
            for width, height in ((6, 12), (8, 16), (8, 32)):
                font = directory / f"fixture-{height}.psf"
                # Each ASCII glyph encodes its character in seven vertical pixels.
                # The harness decodes rendered text independently of the UI tables.
                characters = range(32, 127)
                header = struct.pack("<8I", 0x864AB572, 0, 32, 1, 95, height, height, width)
                bitmap = bytearray()
                for character in characters:
                    bitmap.append(0x80)
                    bitmap.extend(0x80 if character & (1 << bit) else 0 for bit in range(7))
                    bitmap.extend(b"\0" * (height - 8))
                unicode_table = b"".join(bytes((character, 0xFF)) for character in characters)
                font.write_bytes(header + bitmap + unicode_table)
                fonts.append(font)
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(aport),
                    str(ROOT / "tests/host_tool/terminal/fplinux-terminal-engine.c"),
                    str(aport / "terminal-engine.c"),
                    str(aport / "terminal-input.c"),
                    str(aport / "terminal-help.c"),
                    str(aport / "terminal-pty.c"),
                    str(aport / "terminal-render.c"),
                    str(aport / "fplinux-font.c"),
                    str(aport / "fplinux-multitap.c"),
                    str(aport / "fplinux-language.c"),
                    "-ltsm",
                ],
                name="compile terminal engine harness",
                check=True,
            )
            yield TerminalEngineHarness(
                executable, aport / "fplinux-terminal.bashrc", tuple(fonts)
            )

    def run_scenario(self, harness: TerminalEngineHarness, scenario: str) -> None:
        """Run one scenario with a fresh working directory, HOME and history file."""
        with tempfile.TemporaryDirectory() as directory:
            environment = os.environ.copy()
            environment.update(
                HOME=directory,
                HISTFILE=str(Path(directory) / ".bash_history"),
                LC_ALL="C.UTF-8",
            )
            result = run_process(
                [
                    str(harness.executable),
                    scenario,
                    str(harness.bashrc),
                    *(str(font) for font in harness.fonts),
                ],
                name=f"run terminal engine scenario {scenario}",
                cwd=Path(directory),
                env=environment,
                timeout=30,
            )
        assert (result.returncode) == (0), result.stderr

    @pytest.mark.parametrize(
        "scenario",
        [
            "grid",
            "composition",
            "pound-focus",
            "menu-softkeys",
            "menu-choosers",
            "help-input",
            "case-holds",
            "modifier-panel",
            "function-keys",
            "scrollback",
        ],
        ids=[
            "grid",
            "composition",
            "pound-focus",
            "menu-softkeys",
            "menu-choosers",
            "help-input",
            "case-holds",
            "modifier-panel",
            "function-keys",
            "scrollback",
        ],
    )
    def test_phone_and_keyboard_input_drive_pty_bytes_menus_and_scrollback(
        self, harness: TerminalEngineHarness, scenario: str
    ) -> None:
        """Engine state and queued PTY bytes follow the documented terminal controls."""
        self.run_scenario(harness, scenario)

    def test_status_preserves_full_utf8_language_indicator_when_it_fits(
        self, harness: TerminalEngineHarness
    ) -> None:
        """A maximum-size indicator is complete on a wide screen and omitted on a narrow one."""
        self.run_scenario(harness, "language-status")

    @pytest.mark.parametrize(
        "scenario",
        ["render-128x160", "render-240x320", "menu-128x160", "menu-240x320", "help-render"],
        ids=[
            "rendernegative-one28x160",
            "render-240x320",
            "menunegative-one28x160",
            "menu-240x320",
            "help-render",
        ],
    )
    def test_memory_surface_shows_text_input_menus_and_help(
        self, harness: TerminalEngineHarness, scenario: str
    ) -> None:
        """Rendering with fixture fonts keeps text, menus and help inside the surface."""
        self.run_scenario(harness, scenario)

    def test_output_wait_preserves_partial_and_coalesced_prompt(
        self, harness: TerminalEngineHarness
    ) -> None:
        """Output waits retain a prompt prefix and a complete queued prompt."""
        self.run_scenario(harness, "prompt-stream")

    def test_host_bash_keeps_line_editing_completion_and_history(
        self, harness: TerminalEngineHarness
    ) -> None:
        """The host Bash, started through the terminal PTY, honors phone editing keys."""
        self.run_scenario(harness, "bash")
