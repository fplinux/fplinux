# SPDX-License-Identifier: 0BSD
"""Notes app listeners with controlled UI and persistence boundaries."""

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

HERE = ROOT / "tests/host_tool/gui"
APP = ROOT / "alpine/aports/fplinux-notes/fplinux-notes.c"


def notes_compile_flags(temporary: Path) -> list[str]:
    """Use the real UI declarations with unused drawing types supplied by a peer."""
    return [
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(temporary),
        "-I",
        str(ROOT / "include"),
        "-I",
        str(ROOT / "include/fplinux"),
    ]


@pytest.fixture(scope="module")
def notes_build_directory(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Compile the application object independently of its external peers."""
    with c_build_directory(tmp_path_factory, "notes-listeners") as temporary:
        lvgl = temporary / "lvgl"
        lvgl.mkdir()
        (lvgl / "lvgl.h").write_bytes((HERE / "notes_lvgl.h").read_bytes())
        app_object = temporary / "notes.o"
        compile_c(
            app_object,
            [
                *notes_compile_flags(temporary),
                "-Dmain=fplinux_notes_application_main",
                "-c",
                str(APP),
            ],
            name="compile Notes application object",
        )
        yield temporary


@pytest.fixture(scope="module")
def notes_executable(notes_build_directory: Path) -> Path:
    """Link the app with controlled UI and persistence boundaries."""
    executable = notes_build_directory / "notes-listeners"
    compile_c(
        executable,
        [
            *notes_compile_flags(notes_build_directory),
            str(HERE / "notes_save.c"),
            str(notes_build_directory / "notes.o"),
            str(ROOT / "lib/fplinux/fplinux-cli.c"),
            *gettext_link_flags(),
        ],
        name="link Notes listener harness",
    )
    return executable


@pytest.mark.parametrize(
    "scenario",
    [
        "set_failure",
        "save_failure",
        "alert_failure",
        "success",
        "empty",
        "unicode",
        "autosave",
        "implicit_failure",
        "back_failure",
        "close_retry",
        "close_discard",
        "published_unsynced",
        "unsynced_close",
        "clean_empty",
        "clean_loaded",
        "last_key_home",
        "save_retry",
        "reverted",
        "activation",
    ],
)
def test_notes_save_listeners(notes_executable: Path, scenario: str) -> None:
    """Draft survives failed Save; retry and implicit saves retain note bytes."""
    result = run_process(
        [str(notes_executable), scenario],
        name=f"Notes listeners: {scenario}",
        timeout=10,
        check=True,
    )
    assert result.stdout == "notes-listeners: scenario passed\n"


@pytest.mark.parametrize("arguments", [["-h"], ["--help"], ["--unknown", "--help"]])
def test_notes_help_precedes_arguments_and_resource_access(
    notes_executable: Path, arguments: list[str]
) -> None:
    result = run_process([str(notes_executable), "cli", *arguments], name="Notes help", timeout=10)
    assert result.returncode == 0
    assert "Usage: fplinux-notes" in result.stdout
    assert result.stderr == ""


@pytest.mark.parametrize("argument", ["--unknown", "unexpected"])
def test_notes_rejects_arguments_before_resource_access(
    notes_executable: Path, argument: str
) -> None:
    result = run_process(
        [str(notes_executable), "cli", argument], name="Notes arguments", timeout=10
    )
    assert result.returncode == 2
    assert result.stdout == ""
    assert "fplinux-notes:" in result.stderr


def test_notes_saves_private_utf8_file_and_reloads(
    notes_build_directory: Path, tmp_path: Path
) -> None:
    """Real prefs persist escaped note bytes; UI state-dir resolution is controlled."""
    executable = notes_build_directory / "notes-storage"
    compile_c(
        executable,
        [
            *notes_compile_flags(notes_build_directory),
            str(HERE / "notes_storage.c"),
            str(notes_build_directory / "notes.o"),
            str(ROOT / "lib/fplinux/fplinux-ui-prefs.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            str(ROOT / "lib/fplinux/fplinux-cli.c"),
            *gettext_link_flags(),
        ],
        name="link Notes with real preferences",
    )
    state_directory = tmp_path / "note-state"
    state_directory.mkdir(mode=0o700)
    result = run_process(
        [str(executable), str(state_directory)],
        name="Notes save and reopen with real preferences",
        timeout=10,
        check=True,
    )
    assert result.stdout == "notes-storage: save and reopen passed\n"
    preferences = state_directory / "preferences"
    expected = "note=Заметка\\nКнига 📖\\rC:\\\\notes\n"  # noqa: RUF001 -- Literal Cyrillic content.
    assert preferences.read_bytes() == expected.encode()
    assert preferences.stat().st_mode & 0o777 == 0o600
    assert state_directory.stat().st_mode & 0o777 == 0o700
