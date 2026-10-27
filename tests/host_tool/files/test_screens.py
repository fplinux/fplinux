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
    bar_heights = {(128, 128): 32, (128, 160): 32, (176, 220): 42, (240, 320): 52, (320, 240): 44}
    expected_bytes = width * (height - bar_heights[screen]) * 2
    states = ["directory", "menu", "input", "results", "destination", "progress", "system-menu"]
    frames = [(tmp_path / f"files-{state}.rgb565").read_bytes() for state in states]
    assert all(len(frame) == expected_bytes for frame in frames)
    assert len(set(frames)) == len(states)


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
