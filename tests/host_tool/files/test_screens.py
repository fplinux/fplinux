# SPDX-License-Identifier: GPL-2.0-only
"""Actual Files and LVGL with controlled Wayland/session peers, not phone evidence."""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.host_tool.gui.list_rows import ListArea, frame_pixels, sequence_faults
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

HERE = ROOT / "tests/host_tool/files"


@pytest.fixture
def populated_home(tmp_path: Path) -> Iterator[Path]:
    files = tmp_path / "Files"
    files.mkdir()
    (files / "a-dir").mkdir()
    (files / "b-dir").mkdir()
    (files / "large.bin").write_bytes(b"Complete copy\x00\xff" * (512 * 1024))
    (files / "unknown.zzz").write_bytes(b"No handler required for listing")
    for number in range(19):
        (files / f"z-{number:02d}-long-visible-name.txt").write_bytes(b"data")
    (files / "ЁЖИК — длинное имя для проверки списка.txt").write_bytes(b"utf8")
    try:
        yield tmp_path
    finally:
        shutil.rmtree(files)


@pytest.fixture(scope="module")
def files_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path, ui_icon_resources: Path
) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "files-native") as directory:
        run_process(
            ["sh", str(HERE / "native/build.sh"), str(ROOT), str(directory), str(ui_host_inputs)],
            name="build actual Files and native frame/session peer",
            timeout=180,
            check=True,
        )
        (directory / "icons").symlink_to(ui_icon_resources, target_is_directory=True)
        yield directory


@pytest.mark.parametrize("screen", [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)])
@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_files_navigation_search_copy_and_lifecycle_on_actual_ui(
    files_native: Path, populated_home: Path, screen: tuple[int, int], theme: str, locale: str
) -> None:
    tmp_path = populated_home
    files = tmp_path / "Files"
    contents = (files / "large.bin").read_bytes()
    width, height = screen
    result = run_process(
        [
            str(files_native / "files-screens"),
            str(files_native / "files"),
            str(HERE / "native/keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(width),
            str(height),
            theme,
            locale,
        ],
        name=f"actual Files at {width}x{height} ({theme}, {locale})",
        env={
            **os.environ,
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
        },
        timeout=25,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / f"files-screens-{width}x{height}-{theme}-{locale}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock", "Files"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "files-screens: no card; parent focus; returning Options; retained search; "
        "explicit copy destination; background progress; close WAIT/Continue; "
        "readonly system view\n"
    )
    assert (files / "large.bin").read_bytes() == contents
    assert (files / "b-dir/large.bin").read_bytes() == contents
    assert not list((files / "b-dir").glob(".fplinux-copy-*"))
    bar_heights = {(128, 128): 32, (128, 160): 34, (176, 220): 43, (240, 320): 52, (320, 240): 41}
    expected_bytes = width * (height - bar_heights[screen]) * 2
    states = ["directory", "menu", "input", "results", "destination", "progress", "system-menu"]
    frames = [(tmp_path / f"files-{state}.rgb565").read_bytes() for state in states]
    assert all(len(frame) == expected_bytes for frame in frames)
    assert len(set(frames)) == len(states)
    # The folder's rows lie below its title band at the ordinary row pitch.
    band, pitch = {
        (128, 128): (25, 24),
        (128, 160): (22, 23),
        (176, 220): (26, 27),
        (240, 320): (28, 33),
        (320, 240): (28, 32),
    }[screen]
    area = ListArea(
        width=width,
        top=band,
        bottom=height - bar_heights[screen],
        pitch=pitch,
        edge=(0, 2),
        divider=0xBE39 if theme == "light" else 0x3A29,
        highlight=0x3B0E if theme == "light" else 0xAE7B,
    )
    names = ["files-directory"]
    names += [f"files-rows-down-{row:02}" for row in range(1, 24)]
    names += [f"files-rows-up-{row:02}" for row in range(22, -1, -1)]
    rows = {
        name: frame_pixels((tmp_path / f"{name}.rgb565").read_bytes(), width) for name in names
    }
    assert sequence_faults(rows, area) == []


def test_files_stop_and_close_waits_for_worker_cleanup(
    files_native: Path, populated_home: Path
) -> None:
    result = run_process(
        [
            str(files_native / "files-screens"),
            str(files_native / "files"),
            str(HERE / "native/keyboard.xkb"),
            str(populated_home / "session.sock"),
            str(populated_home),
            "240",
            "320",
            "dark",
            "en",
            "stop",
        ],
        name="actual Files asynchronous Stop and close",
        env={
            **os.environ,
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
        },
        timeout=15,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "files-stop: repeated Close has one decision; asynchronous cancellation "
        "cleans the unpublished copy before READY\n"
    )
    files = populated_home / "Files"
    assert (files / "large.bin").read_bytes() == b"Complete copy\x00\xff" * (512 * 1024)
    assert not list((files / "b-dir").iterdir())


def test_files_repeated_conflict_reuses_draft_and_cancel_releases_open_protection(
    files_native: Path, populated_home: Path
) -> None:
    files = populated_home / "Files"
    source = (files / "large.bin").read_bytes()
    (files / "large.binz").write_bytes(b"Existing alternate name")
    result = run_process(
        [
            str(files_native / "files-screens"),
            str(files_native / "files"),
            str(HERE / "native/keyboard.xkb"),
            str(populated_home / "session.sock"),
            str(populated_home),
            "240",
            "320",
            "dark",
            "en",
            "conflict",
        ],
        name="actual Files repeated conflict, retained draft and cancelled destination",
        env={
            **os.environ,
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
        },
        timeout=20,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "files-conflict: repeated Another name reuses the draft; OPEN retains editing; "
        "cancelling editor or destination clears protection and preserves source selection\n"
    )
    assert (files / "large.bin").read_bytes() == source
    assert (files / "large.binz").read_bytes() == b"Existing alternate name"
    assert not list(files.glob(".fplinux-copy-*"))


def run_scenario(files_native: Path, home: Path, scenario: str, *, name: str) -> str:
    """Run one native scenario at 240x320 in English and return its report."""
    result = run_process(
        [
            str(files_native / "files-screens"),
            str(files_native / "files"),
            str(HERE / "native/keyboard.xkb"),
            str(home / "session.sock"),
            str(home),
            "240",
            "320",
            "dark",
            "en",
            scenario,
        ],
        name=name,
        env={
            **os.environ,
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
        },
        timeout=25,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def test_files_folder_changes_show_one_screen_and_keep_focus(
    files_native: Path, populated_home: Path
) -> None:
    report = run_scenario(
        files_native, populated_home, "transitions", name="actual Files folder transitions"
    )
    steps = dict(line.split(": ", 1) for line in report.splitlines())
    assert set(steps) == {"open", "refresh", "subfolder", "back"}, report
    problems = [
        step
        for step, step_report in steps.items()
        if "waiting=0 " not in step_report or "[||Cancel]" in step_report
    ]
    if not steps["refresh"].endswith(" focus=2"):
        problems.append("refresh focus")
    assert not problems, report


def test_files_refresh_delete_and_rename_keep_focus_and_first_visible_row(
    files_native: Path, populated_home: Path
) -> None:
    report = run_scenario(
        files_native, populated_home, "positions", name="actual Files list positions after changes"
    )
    assert report == (
        "files-positions: Refresh, Delete and Rename keep the focused name or its row "
        "and the first visible row; a change from search results keeps the query\n"
    )
    files = populated_home / "Files"
    assert not (files / "zz-new.txt").exists()
    assert not (files / "z-13-long-visible-name.txt").exists()
    assert not (files / "z-14-long-visible-name.txt").exists()
    assert (files / "z-14-long-visible-name.txtz").read_bytes() == b"data"


def test_files_cancelled_copy_returns_to_source_without_error(
    files_native: Path, populated_home: Path
) -> None:
    files = populated_home / "Files"
    contents = (files / "large.bin").read_bytes()
    report = run_scenario(
        files_native, populated_home, "cancel-copy", name="actual Files cancelled copy"
    )
    assert report == (
        "files-cancel-copy: Cancel stops the copy and returns to the source without an error\n"
    )
    assert (files / "large.bin").read_bytes() == contents
    assert not list((files / "b-dir").iterdir())


def test_files_close_question_survives_completed_copy(
    files_native: Path, populated_home: Path
) -> None:
    files = populated_home / "Files"
    contents = (files / "large.bin").read_bytes()
    report = run_scenario(
        files_native, populated_home, "question", name="actual Files close question after a copy"
    )
    assert report == (
        "files-question: Close Files? stays after the copy ends; Continue then shows the source\n"
    )
    assert (files / "b-dir/large.bin").read_bytes() == contents
    assert not list((files / "b-dir").glob(".fplinux-copy-*"))


def test_files_places_keep_focus_after_connect_changes_card_id(
    files_native: Path, populated_home: Path
) -> None:
    report = run_scenario(
        files_native, populated_home, "places", name="actual Files Places with a system card"
    )
    assert report == (
        "files-places: no system card row; My files on the memory card; "
        "Connect waits and keeps focus after the card ID changes\n"
    )


def test_files_places_keep_selected_card_and_menu_when_storage_answers(
    files_native: Path, populated_home: Path
) -> None:
    card = populated_home / "card"
    card.mkdir()
    (card / "card-note.txt").write_bytes(b"On the card")
    report = run_scenario(
        files_native,
        populated_home,
        "places-continuity",
        name="actual Files Places continuity and memory card root",
    )
    assert report == (
        "files-places-continuity: Places keeps the card; unchanged and reordered snapshots "
        "keep Options; Help keeps the deferred snapshot; Eject keeps focus after the card ID "
        "changes; card root has its place name\n"
    )
    assert (card / "card-note.txt").read_bytes() == b"On the card"


@pytest.mark.parametrize("entry", ["refresh", "open"])
def test_files_removed_card_reports_once_after_storage_answers(
    files_native: Path, populated_home: Path, entry: str
) -> None:
    card = populated_home / "card"
    card.mkdir()
    (card / "card-note.txt").write_bytes(b"On the card")
    report = run_scenario(
        files_native,
        populated_home,
        "removed-card" if entry == "refresh" else "removed-card-open",
        name=f"actual Files removed memory card during {entry}",
    )
    assert report == (
        "files-removed-card: failed reading waits for storage; one error over Places closes once\n"
    )
    assert (populated_home / "card-away/card-note.txt").read_bytes() == b"On the card"


@pytest.mark.parametrize("action", ["copy", "move"])
def test_files_completed_transfer_returns_to_source_with_search_focus_and_scroll(
    files_native: Path, populated_home: Path, action: str
) -> None:
    report = run_scenario(
        files_native,
        populated_home,
        f"{action}-return",
        name=f"actual Files source context after {action}",
    )
    assert report == (
        "files-return-transfer: completion returns to the source; folder and search keep "
        "focus and scroll; Back keeps the query\n"
    )
    files = populated_home / "Files"
    assert (files / "b-dir/z-13-long-visible-name.txt").read_bytes() == b"data"
    if action == "copy":
        assert (files / "z-13-long-visible-name.txt").read_bytes() == b"data"
    else:
        assert not (files / "z-13-long-visible-name.txt").exists()


def test_files_symlink_and_special_file_centre_command_says_details(
    files_native: Path, populated_home: Path
) -> None:
    files = populated_home / "Files"
    (files / "a-link").symlink_to(files / "large.bin")
    os.mkfifo(files / "a-pipe")
    report = run_scenario(
        files_native, populated_home, "special", name="actual Files inspecting special files"
    )
    assert report == (
        "files-special: Centre says Details for symbolic links and special files "
        "and opens their information\n"
    )


def test_files_slow_folder_shows_busy_alert_after_delay_and_back_keeps_folder(
    files_native: Path, populated_home: Path
) -> None:
    report = run_scenario(
        files_native, populated_home, "slow", name="actual Files slow folder reading"
    )
    assert report == (
        "files-slow: the busy alert waits, then stays; Back or Cancel keeps the folder\n"
    )


@pytest.mark.parametrize("arguments", [["--help"], ["--unknown", "--help"]])
def test_files_help_returns_before_connecting_to_session(
    files_native: Path, arguments: list[str]
) -> None:
    result = run_process(
        [str(files_native / "files"), *arguments], name="Files help without a session", timeout=5
    )
    assert result.returncode == 0
    assert "Usage: fplinux-files" in result.stdout
    assert result.stderr == ""
