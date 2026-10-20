# SPDX-License-Identifier: GPL-2.0-only
"""Host metadata reader contracts with controlled files and real executable checks."""

from __future__ import annotations

import os
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def desktop_reader(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "desktop-entry") as directory:
        executable = directory / "desktop-reader"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include/fplinux"),
                str(ROOT / "tests/host_tool/gui/fplinux-desktop-entry-test.c"),
                str(ROOT / "lib/fplinux/fplinux-desktop-entry.c"),
            ],
            name="compile trusted desktop metadata reader",
        )
        yield executable


def write_entry(
    directory: Path,
    executable: Path,
    *,
    app_id: str = "org.example.notes",
    fields: str = "",
    mode: str = "integrated",
) -> Path:
    path = directory / f"{app_id}.desktop"
    path.write_text(
        f"[Desktop Entry]\nType=Application\nName=Notes\nExec={executable}\n"
        f"X-FPLinux-Mode={mode}\n{fields}",
        encoding="utf-8",
    )
    path.chmod(0o600)
    return path


def read_entry(
    reader: Path,
    directory: Path,
    *,
    app_id: str = "org.example.notes",
    uid: int | None = None,
    locale: str | None = None,
) -> dict[str, str]:
    command = [str(reader), str(directory), app_id, str(os.getuid() if uid is None else uid)]
    if locale is not None:
        command.append(locale)
    result = run_process(
        command,
        name="read trusted desktop metadata",
        timeout=5,
        check=True,
    )
    return dict(line.split("=", 1) for line in result.stdout.splitlines())


@pytest.mark.parametrize(("mode", "expected"), [("integrated", "1"), ("fullscreen", "2")])
def test_desktop_reader_admits_installed_application(
    desktop_reader: Path, tmp_path: Path, mode: str, expected: str
) -> None:
    write_entry(tmp_path, desktop_reader, mode=mode)
    result = read_entry(desktop_reader, tmp_path)
    assert result["valid"] == "1"
    assert result["app_id"] == "org.example.notes"
    assert result["title"] == "Notes"
    assert result["executable"] == str(desktop_reader)
    assert result["mode"] == expected


@pytest.mark.parametrize(
    "fields",
    ["Name=Notes\nName[ru]=Заметки\n", "Name[ru]=Заметки\nName=Notes\n"],
    ids=["base-first", "localized-first"],
)
def test_desktop_reader_selects_localized_title_in_either_field_order(
    desktop_reader: Path, tmp_path: Path, fields: str
) -> None:
    path = write_entry(tmp_path, desktop_reader, fields=fields)
    path.write_text(path.read_text().replace("Name=Notes\n", "", 1), encoding="utf-8")
    result = read_entry(desktop_reader, tmp_path, locale="ru")
    assert result["valid"] == "1"
    assert result["title"] == "Заметки"


@pytest.mark.parametrize(
    ("fields", "locale"),
    [
        ("", "ru"),
        ("Name[ru]=\n", "ru"),
        ("Name[ru]=Заметки\n", "en"),
        ("Name[ru]=Заметки\n", None),
        ("Name[ru]=Заметки\nName[ru]=\n", "ru"),
    ],
    ids=["missing", "empty", "other-language", "base-only", "last-empty"],
)
def test_desktop_reader_uses_base_title_without_nonempty_selected_name(
    desktop_reader: Path, tmp_path: Path, fields: str, locale: str | None
) -> None:
    write_entry(tmp_path, desktop_reader, fields=fields)
    result = read_entry(desktop_reader, tmp_path, locale=locale)
    assert result["valid"] == "1"
    assert result["title"] == "Notes"


@pytest.mark.parametrize("name", [None, "", "a" * 128], ids=["missing", "empty", "oversized"])
def test_desktop_reader_requires_valid_base_name_with_localized_title(
    desktop_reader: Path, tmp_path: Path, name: str | None
) -> None:
    path = write_entry(tmp_path, desktop_reader, fields="Name[ru]=Заметки\n")
    name_field = "" if name is None else f"Name={name}\n"
    path.write_text(path.read_text().replace("Name=Notes\n", name_field, 1), encoding="utf-8")
    assert read_entry(desktop_reader, tmp_path, locale="ru")["valid"] == "0"


@pytest.mark.parametrize("later_name", ["", "Name[ru]=Заметки\n"], ids=["single", "later-valid"])
def test_desktop_reader_rejects_oversized_selected_localized_name(
    desktop_reader: Path, tmp_path: Path, later_name: str
) -> None:
    write_entry(tmp_path, desktop_reader, fields="Name[ru]=" + "a" * 128 + "\n" + later_name)
    assert read_entry(desktop_reader, tmp_path, locale="ru")["valid"] == "0"


@pytest.mark.parametrize(
    ("app_id", "mode", "expected"),
    [
        ("org.example.notes", "launcher", "0"),
        ("org.fplinux.launcher", "integrated", "0"),
        ("org.fplinux.launcher", "launcher", "1"),
    ],
)
def test_launcher_identity_requires_launcher_mode(
    desktop_reader: Path, tmp_path: Path, app_id: str, mode: str, expected: str
) -> None:
    write_entry(tmp_path, desktop_reader, app_id=app_id, mode=mode)
    result = read_entry(desktop_reader, tmp_path, app_id=app_id)
    assert result["valid"] == expected
    if expected == "1":
        assert result["mode"] == "0"


@pytest.mark.parametrize("permissions", [0o664, 0o646, 0o666])
def test_desktop_reader_rejects_writable_metadata(
    desktop_reader: Path, tmp_path: Path, permissions: int
) -> None:
    path = write_entry(tmp_path, desktop_reader)
    path.chmod(permissions)
    assert read_entry(desktop_reader, tmp_path)["valid"] == "0"


def test_desktop_reader_requires_owner_and_regular_metadata(
    desktop_reader: Path, tmp_path: Path
) -> None:
    path = write_entry(tmp_path, desktop_reader)
    assert read_entry(desktop_reader, tmp_path, uid=os.getuid() + 1)["valid"] == "0"
    target = tmp_path / "trusted"
    path.rename(target)
    path.symlink_to(target)
    assert read_entry(desktop_reader, tmp_path)["valid"] == "0"
    path.unlink()
    path.mkdir()
    assert read_entry(desktop_reader, tmp_path)["valid"] == "0"
    path.rmdir()
    assert read_entry(desktop_reader, tmp_path)["valid"] == "0"


@pytest.mark.parametrize("character", [" ", "\t", "%", "\\", '"', "'", chr(96), "$"])
def test_exec_rejects_shell_syntax_in_real_executable_name(
    desktop_reader: Path, tmp_path: Path, character: str
) -> None:
    executable = tmp_path / f"notes{character}app"
    executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    executable.chmod(0o755)
    write_entry(tmp_path, executable)
    assert read_entry(desktop_reader, tmp_path)["valid"] == "0"


def test_exec_requires_absolute_existing_executable(desktop_reader: Path, tmp_path: Path) -> None:
    executable = tmp_path / "notes"
    executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    executable.chmod(0o644)
    write_entry(tmp_path, executable)
    assert read_entry(desktop_reader, tmp_path)["valid"] == "0"
    executable.unlink()
    assert read_entry(desktop_reader, tmp_path)["valid"] == "0"
    path = write_entry(tmp_path, desktop_reader)
    path.write_text(path.read_text().replace(str(desktop_reader), "notes"), encoding="utf-8")
    assert read_entry(desktop_reader, tmp_path)["valid"] == "0"


@pytest.mark.parametrize("app_id", ["", "a" * 128, "../escape", "two words", "bad$"])
def test_desktop_reader_rejects_invalid_identity(
    desktop_reader: Path, tmp_path: Path, app_id: str
) -> None:
    result = read_entry(desktop_reader, tmp_path, app_id=app_id)
    assert result["id_valid"] == "0"
    assert result["valid"] == "0"


def test_desktop_identity_accepts_ascii_at_127_byte_limit(
    desktop_reader: Path, tmp_path: Path
) -> None:
    app_id = "Ab9._-" + "a" * 121
    write_entry(tmp_path, desktop_reader, app_id=app_id)
    result = read_entry(desktop_reader, tmp_path, app_id=app_id)
    assert result["id_valid"] == "1"
    assert result["valid"] == "1"
    assert result["app_id"] == app_id


@pytest.mark.parametrize(
    "fields",
    [
        "Name=" + "a" * 128 + "\nName=Notes\n",
        "X-FPLinux-Mode=invalid\nX-FPLinux-Mode=integrated\n",
    ],
)
def test_later_valid_value_cannot_hide_invalid_field(
    desktop_reader: Path, tmp_path: Path, fields: str
) -> None:
    write_entry(tmp_path, desktop_reader, fields=fields)
    assert read_entry(desktop_reader, tmp_path)["valid"] == "0"


def test_last_valid_name_and_mode_win(desktop_reader: Path, tmp_path: Path) -> None:
    write_entry(tmp_path, desktop_reader, fields="Name=Last\nX-FPLinux-Mode=fullscreen\n")
    result = read_entry(desktop_reader, tmp_path)
    assert result["valid"] == "1"
    assert result["title"] == "Last"
    assert result["mode"] == "2"


def test_metadata_callback_sees_only_desktop_group_and_can_reject(
    desktop_reader: Path, tmp_path: Path
) -> None:
    write_entry(
        tmp_path, desktop_reader, fields="# Reject=true\nExtra=one\n[Other]\nReject=true\n"
    )
    result = read_entry(desktop_reader, tmp_path)
    assert result["valid"] == "1"
    assert result["extra_fields"] == "1"
    write_entry(tmp_path, desktop_reader, fields="Reject=true\n")
    assert read_entry(desktop_reader, tmp_path)["valid"] == "0"


def test_reader_accepts_crlf_and_unterminated_final_line(
    desktop_reader: Path, tmp_path: Path
) -> None:
    path = write_entry(tmp_path, desktop_reader)
    text = path.read_text().replace("\n", "\r\n").rstrip("\r\n")
    path.write_bytes(text.encode())
    assert read_entry(desktop_reader, tmp_path)["valid"] == "1"


@pytest.mark.parametrize(
    "trailing_field",
    ["Name=Injected", "Exec=/bin/false", "X-FPLinux-Mode=fullscreen", "Extra=injected"],
    ids=["title", "command", "mode", "callback"],
)
def test_reader_keeps_overlong_comment_tail_out_of_desktop_metadata(
    desktop_reader: Path, tmp_path: Path, trailing_field: str
) -> None:
    write_entry(tmp_path, desktop_reader, fields="#" + "a" * 65536 + trailing_field + "\n")
    result = read_entry(desktop_reader, tmp_path)
    assert result["extra_fields"] == "0"
    if result["valid"] == "1":
        assert result["title"] == "Notes"
        assert result["executable"] == str(desktop_reader)
        assert result["mode"] == "1"
    else:
        assert result["valid"] == "0"
