# SPDX-License-Identifier: GPL-2.0-only
"""Whole Terminal callbacks with real input/session code and host boundary doubles."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.gettext_support import gettext_link_flags
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-terminal"
PEERS = ROOT / "tests/host_tool/gui"


@pytest.fixture(scope="module")
def terminal_app(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the application separately; replace its UI, drawing and PTY boundaries."""
    with c_build_directory(tmp_path_factory, "terminal-app") as directory:
        application = directory / "terminal-app.o"
        flags = [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(PEERS / "terminal-app-boundary"),
            "-I",
            str(ROOT / "include"),
            "-I",
            str(ROOT / "include/fplinux"),
            "-I",
            str(APORT),
        ]
        compile_c(
            application,
            [*flags, "-Dmain=fplinux_terminal_main", "-c", str(APORT / "fplinux-terminal.c")],
            name="compile Terminal application object",
        )
        executable = directory / "terminal-app"
        compile_c(
            executable,
            [
                *flags,
                str(PEERS / "terminal-app-test.c"),
                str(application),
                str(APORT / "terminal-engine.c"),
                str(APORT / "terminal-input.c"),
                str(APORT / "terminal-session.c"),
                str(APORT / "terminal-help.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                str(ROOT / "lib/fplinux/fplinux-language.c"),
                "-Wl,--wrap=fplinux_language_list",
                "-ltsm",
                *gettext_link_flags(),
            ],
            name="link Terminal application callback harness",
        )
        yield executable


@pytest.mark.parametrize("arguments", [["-h"], ["--help"], ["--unknown", "--help"]])
def test_terminal_help_precedes_arguments_and_resource_access(
    terminal_app: Path, arguments: list[str]
) -> None:
    result = run_process([str(terminal_app), "cli", *arguments], name="Terminal help", timeout=10)
    assert result.returncode == 0
    assert "Usage: fplinux-terminal" in result.stdout
    assert result.stderr == ""


@pytest.mark.parametrize("argument", ["--unknown", "unexpected"])
def test_terminal_rejects_arguments_before_resource_access(
    terminal_app: Path, argument: str
) -> None:
    result = run_process(
        [str(terminal_app), "cli", argument], name="Terminal arguments", timeout=10
    )
    assert result.returncode == 2
    assert result.stdout == ""
    assert "fplinux-terminal:" in result.stderr


def test_close_during_ui_open_starts_no_shell(terminal_app: Path) -> None:
    _run_application(terminal_app, "close-during-open")


@pytest.mark.parametrize("scenario", ["visibility", "options-menu"])
def test_hidden_or_covered_terminal_preserves_shell_and_pauses_phone_input(
    terminal_app: Path, scenario: str
) -> None:
    _run_application(terminal_app, scenario)


def test_keyboard_function_keys_are_shell_input_and_modifiers_keep_phone_preedit(
    terminal_app: Path,
) -> None:
    _run_application(terminal_app, "keyboard")


def test_opening_options_cancels_a_queued_hold_before_pty_writes_resume(
    terminal_app: Path,
) -> None:
    _run_application(terminal_app, "backpressure-options")


def test_resuming_covered_input_samples_expiration_time_after_unpause(terminal_app: Path) -> None:
    _run_application(terminal_app, "resume-clock")


def test_hidden_synthetic_releases_do_not_switch_mode_or_clear_the_shell_line(
    terminal_app: Path,
) -> None:
    _run_application(terminal_app, "hidden-releases")


def test_input_cancellation_preserves_text_and_discards_only_phone_hold_gestures(
    terminal_app: Path,
) -> None:
    _run_application(terminal_app, "input-cancel")


def test_shell_backpressure_preserves_keyboard_text_before_enter(terminal_app: Path) -> None:
    _run_application(terminal_app, "backpressure")


def _run_application(executable: Path, scenario: str) -> None:
    result = run_process(
        [str(executable), scenario],
        name=f"run Terminal application {scenario}",
        timeout=10,
        check=True,
    )
    assert result.stdout == f"terminal-app {scenario}: OK\n"
    assert result.stderr == ""
