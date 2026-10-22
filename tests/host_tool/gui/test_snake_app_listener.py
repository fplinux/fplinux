# SPDX-License-Identifier: GPL-2.0-only
"""Snake app listeners and geometry against host UI and drawing doubles."""

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

APORT = ROOT / "alpine/aports/fplinux-snake"
PEERS = ROOT / "tests/host_tool/gui"


@pytest.fixture(scope="module")
def snake_app(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the app normally; replace only external UI and preferences APIs."""
    with c_build_directory(tmp_path_factory, "snake-app-listener") as directory:
        application = directory / "snake-app.o"
        flags = [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(PEERS / "snake-boundary"),
            "-I",
            str(ROOT / "include"),
            "-I",
            str(ROOT / "include/fplinux"),
        ]
        compile_c(
            application,
            [*flags, "-Dmain=fplinux_snake_app_main", "-c", str(APORT / "fplinux-snake.c")],
            name="compile Snake application object",
        )
        executable = directory / "snake-app-listener"
        compile_c(
            executable,
            [
                *flags,
                str(PEERS / "snake_app_listener.c"),
                str(application),
                str(APORT / "snake.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                *gettext_link_flags(),
            ],
            name="link Snake app listener harness",
        )
        yield executable


@pytest.mark.parametrize(
    ("screen", "bars", "board"),
    [
        ((128, 128), (16, 16), (1, 16, 126, 96)),
        ((128, 160), (16, 16), (1, 17, 126, 126)),
        ((176, 220), (18, 24), (0, 19, 176, 176)),
        ((320, 240), (22, 22), (0, 24, 320, 192)),
        ((320, 160), (16, 16), (0, 16, 320, 128)),
        ((240, 320), (28, 28), (0, 28, 240, 264)),
    ],
    ids=[
        "square",
        "small-portrait",
        "medium-portrait",
        "landscape",
        "wide-short",
        "large-portrait",
    ],
)
def test_board_uses_independent_runtime_dimensions(
    snake_app: Path,
    screen: tuple[int, int],
    bars: tuple[int, int],
    board: tuple[int, int, int, int],
) -> None:
    """Actual app creation yields a contained board with literal extents."""
    result = run_process(
        [str(snake_app), "geometry", *(str(value) for value in (*screen, *bars, *board))],
        name="run Snake app geometry harness",
        timeout=10,
        check=True,
    )
    assert result.stdout == "snake-app-listener: all checks passed\n"


@pytest.mark.parametrize("scenario", ["listener", "close-playing"])
def test_hidden_home_and_close_pause_and_save_scores(snake_app: Path, scenario: str) -> None:
    """Events stop movement and save scores at the app's lifecycle boundary."""
    result = run_process(
        [str(snake_app), scenario, "128", "160", "16", "16", "1", "17", "126", "126"],
        name="run Snake app listener harness",
        timeout=10,
        check=True,
    )
    assert result.stdout == "snake-app-listener: all checks passed\n"


def test_close_during_ui_open_saves_scores_before_view_creation(snake_app: Path) -> None:
    """The UI boundary can deliver CLOSE synchronously before open returns."""
    result = run_process(
        [str(snake_app), "close-during-open", "128", "160", "16", "16", "1", "17", "126", "126"],
        name="run Snake close during UI open harness",
        timeout=10,
        check=True,
    )
    assert result.stdout == "snake-app-listener: all checks passed\n"


def test_score_write_failure_is_visible_and_keeps_play_and_back_available(snake_app: Path) -> None:
    result = run_process(
        [str(snake_app), "save-failure", "128", "160", "16", "16", "1", "17", "126", "126"],
        name="run Snake failed score save listener harness",
        timeout=10,
        check=True,
    )
    assert result.stdout == "snake-app-listener: all checks passed\n"


@pytest.mark.parametrize("arguments", [["-h"], ["--help"], ["--unknown", "--help"]])
def test_snake_help_precedes_arguments_and_resource_access(
    snake_app: Path, arguments: list[str]
) -> None:
    result = run_process([str(snake_app), "cli", *arguments], name="Snake help", timeout=10)
    assert result.returncode == 0
    assert "Usage: fplinux-snake" in result.stdout
    assert result.stderr == ""


@pytest.mark.parametrize("argument", ["--unknown", "unexpected"])
def test_snake_rejects_arguments_before_resource_access(snake_app: Path, argument: str) -> None:
    result = run_process([str(snake_app), "cli", argument], name="Snake arguments", timeout=10)
    assert result.returncode == 2
    assert result.stdout == ""
    assert "fplinux-snake:" in result.stderr
