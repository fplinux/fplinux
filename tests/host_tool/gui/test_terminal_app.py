# SPDX-License-Identifier: GPL-2.0-only
"""Whole Terminal callbacks with real input/session code and host boundary doubles."""

from __future__ import annotations

import errno
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.gettext_support import gettext_link_flags
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

APORT = ROOT / "alpine/aports/fplinux-terminal"
PEERS = ROOT / "tests/host_tool/gui"
LANGUAGES = ROOT / "tests/host_tool/input/fixtures/input-languages"


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
            f'-DFPLINUX_TERMINAL_TEST_LANGUAGE_DIRECTORY="{LANGUAGES}"',
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
                str(ROOT / "lib/fplinux/fplinux-text-input.c"),
                str(ROOT / "lib/fplinux/fplinux-language.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-prefs.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                "-Wl,--wrap=fplinux_language_list",
                "-Wl,--wrap=fplinux_language_load",
                "-Wl,--wrap=fplinux_ui_prefs_save",
                "-Wl,--wrap=ioctl",
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


def test_shared_symbol_picker_and_clear_consume_commands_and_receive_releases(
    terminal_app: Path,
) -> None:
    _run_application(terminal_app, "shared-keypad")


def test_opening_options_cancels_a_queued_hold_before_pty_writes_resume(
    terminal_app: Path,
) -> None:
    _run_application(terminal_app, "backpressure-options")


def test_resuming_covered_input_samples_expiration_time_after_unpause(terminal_app: Path) -> None:
    _run_application(terminal_app, "resume-clock")


def test_hidden_synthetic_releases_cancel_digit_and_clear_holds(
    terminal_app: Path,
) -> None:
    _run_application(terminal_app, "hidden-releases")


def test_input_cancellation_preserves_text_and_discards_only_phone_hold_gestures(
    terminal_app: Path,
) -> None:
    _run_application(terminal_app, "input-cancel")


@pytest.mark.parametrize("scenario", ["backpressure", "backpressure-hidden"])
def test_shell_backpressure_drains_keyboard_text_before_enter_without_timer(
    terminal_app: Path, scenario: str
) -> None:
    _run_application(terminal_app, scenario)


def _run_application(
    executable: Path,
    scenario: str,
    *arguments: str,
    state_directory: Path | None = None,
    expected_error: str = "",
) -> None:
    with TemporaryDirectory(prefix="state-", dir=executable.parent) as temporary:
        result = run_process(
            [str(executable), scenario, *arguments],
            name=f"run Terminal application {scenario}",
            env={"FPLINUX_TERMINAL_TEST_STATE": str(state_directory or temporary)},
            timeout=10,
            check=True,
        )
    assert result.stdout == f"terminal-app {scenario}: OK\n"
    assert result.stderr == expected_error


@pytest.mark.parametrize(
    ("width", "height", "columns", "rows"),
    [(128, 128, 21, 9), (240, 268, 30, 15), (320, 196, 40, 10)],
)
def test_terminal_default_font_fills_the_grid_above_the_status_bar(
    terminal_app: Path, width: int, height: int, columns: int, rows: int
) -> None:
    _run_application(terminal_app, "font-default", *map(str, (width, height, columns, rows)))


@pytest.mark.parametrize(
    ("size", "choice", "grid"),
    [
        ((128, 128), "8x16", (16, 6)),
        ((240, 268), "6x12", (40, 20)),
        ((320, 196), "6x12", (53, 14)),
    ],
)
def test_terminal_explicit_font_preference_wins_over_the_profile_default(
    terminal_app: Path,
    tmp_path: Path,
    size: tuple[int, int],
    choice: str,
    grid: tuple[int, int],
) -> None:
    width, height = size
    columns, rows = grid
    (tmp_path / "preferences").write_text(f"font_size={choice}\n", encoding="utf-8")
    _run_application(
        terminal_app,
        "font-default",
        *map(str, (width, height, columns, rows)),
        state_directory=tmp_path,
    )


def test_terminal_unknown_font_preference_uses_the_profile_default(
    terminal_app: Path, tmp_path: Path
) -> None:
    (tmp_path / "preferences").write_text("font_size=unknown\n", encoding="utf-8")
    _run_application(
        terminal_app, "font-default", "240", "268", "30", "15", state_directory=tmp_path
    )


def test_terminal_live_font_change_preserves_screen_input_and_saved_choice(
    terminal_app: Path, tmp_path: Path
) -> None:
    _run_application(terminal_app, "font-change", state_directory=tmp_path)
    assert (tmp_path / "preferences").read_text(encoding="utf-8") == "font_size=8x16\n"
    _run_application(
        terminal_app, "font-default", "128", "128", "16", "6", state_directory=tmp_path
    )


def test_terminal_font_save_failure_keeps_shell_and_pending_input_alive(
    terminal_app: Path, tmp_path: Path
) -> None:
    _run_application(
        terminal_app,
        "font-save-failure",
        state_directory=tmp_path,
        expected_error="terminal: save font size: Read-only file system\n",
    )
    assert not (tmp_path / "preferences").exists()


def test_terminal_published_font_without_sync_keeps_live_choice_and_reports_uncertainty(
    terminal_app: Path, tmp_path: Path
) -> None:
    _run_application(
        terminal_app,
        "font-sync-failure",
        state_directory=tmp_path,
        expected_error=f"terminal: save font size: {os.strerror(errno.EIO)}\n",
    )
    assert (tmp_path / "preferences").read_text(encoding="utf-8") == "font_size=8x16\n"


def test_terminal_starts_with_defaults_when_new_preferences_cannot_be_written(
    terminal_app: Path,
) -> None:
    _run_application(
        terminal_app,
        "font-default",
        "128",
        "128",
        "21",
        "9",
        state_directory=Path("/proc/fplinux-terminal-unavailable-state"),
    )


def test_terminal_status_keeps_mode_modifiers_and_history_visible_on_a_narrow_screen(
    terminal_app: Path,
) -> None:
    _run_application(terminal_app, "status")


def test_terminal_unreadable_preferences_show_an_alert_before_starting_a_shell(
    terminal_app: Path,
) -> None:
    _run_application(
        terminal_app,
        "prefs-open-failure",
        expected_error="terminal: load preferences: Permission denied\n",
    )
