# SPDX-License-Identifier: GPL-2.0-only
"""Linked terminal input and screen-command rules with real libtsm encoding."""

from __future__ import annotations

import gettext
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.gettext_support import gettext_link_flags
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def terminal_input(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "terminal-input") as directory:
        executable = directory / "terminal-input"
        aport = ROOT / "alpine/aports/fplinux-terminal"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{aport}",
                str(ROOT / "tests/host_tool/gui/terminal-input-test.c"),
                str(aport / "terminal-engine.c"),
                str(aport / "terminal-input.c"),
                str(aport / "terminal-help.c"),
                str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                str(ROOT / "lib/fplinux/fplinux-text-input.c"),
                str(ROOT / "lib/fplinux/fplinux-language.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-screen-model.c"),
                "-ltsm",
                *gettext_link_flags(),
            ],
            name="link terminal input and GUI command model",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario", ["languages", "pause", "modifiers", "gestures", "blocked", "commands"]
)
def test_terminal_input_keeps_pty_bytes_deadlines_and_gui_commands(
    terminal_input: Path, scenario: str
) -> None:
    result = run_process(
        [
            str(terminal_input),
            scenario,
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"run terminal {scenario} input contract",
        timeout=10,
        check=True,
    )
    assert result.stdout == f"terminal-input {scenario}: OK\n"


def test_terminal_catalogue_translates_commands_and_background_shell_help(
    terminal_input: Path, tmp_path: Path
) -> None:
    catalogue = tmp_path / "ru/LC_MESSAGES/fplinux-terminal.mo"
    catalogue.parent.mkdir(parents=True)
    run_process(
        [
            "msgfmt",
            "--check",
            "--output-file",
            str(catalogue),
            str(ROOT / "alpine/aports/fplinux-terminal/ru.po"),
        ],
        name="compile Terminal Russian catalogue",
        timeout=10,
        check=True,
    )
    with catalogue.open("rb") as file:
        russian = gettext.GNUTranslations(file)
    assert russian.gettext("Terminal") == "Терминал"
    assert russian.gettext("Special keys") == "Служебные клавиши"
    assert russian.gettext("Font size") == "Размер шрифта"
    help_fixtures = ROOT / "tests/host_tool/gui/fixtures/terminal-help"
    result = run_process(
        [
            str(terminal_input),
            "help",
            str(tmp_path),
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="translate production Terminal exit help",
        timeout=10,
        env={},
        check=True,
    )
    assert result.stdout == (help_fixtures / "ru.txt").read_text(encoding="utf-8")
    assert result.stderr == ""
