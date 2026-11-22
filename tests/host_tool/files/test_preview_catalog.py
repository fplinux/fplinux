# SPDX-License-Identifier: GPL-2.0-only
"""Exercise Files handler discovery through the desktop entry reader."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FILES_APORT = ROOT / "alpine/aports/fplinux-files"


@pytest.fixture(scope="module")
def preview_catalog(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "preview-catalog") as directory:
        executable = directory / "preview-catalog"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include/fplinux"),
                "-I",
                str(FILES_APORT),
                str(ROOT / "tests/host_tool/files/native/preview-catalog.c"),
                str(FILES_APORT / "files-preview-catalog.c"),
                str(ROOT / "lib/fplinux-shared/fplinux-desktop-entry.c"),
            ],
            name="compile Files preview handler catalogue",
        )
        yield executable


def write_handler(
    directory: Path,
    executable: Path,
    *,
    app_id: str = "org.example.games",
    extensions: str = "jar;",
    mode: str = "integrated",
    fields: str = "Icon=game-symbolic\n",
) -> Path:
    path = directory / f"{app_id}.desktop"
    path.write_text(
        f"[Desktop Entry]\nType=Application\nName=Games\nExec={executable}\n"
        f"X-FPLinux-Mode={mode}\nX-FPLinux-FileExtensions={extensions}\n{fields}",
        encoding="utf-8",
    )
    path.chmod(0o600)
    return path


def check_handler(
    checker: Path,
    directory: Path,
    path: str,
    *,
    found: bool,
    icon: str = "",
    program: Path | None = None,
) -> None:
    run_process(
        [
            str(checker),
            str(directory),
            path,
            "match" if found else "none",
            icon,
            "" if program is None else str(program),
        ],
        name="match Files preview handler",
        cwd=directory,
        timeout=5,
        check=True,
    )


def test_preview_catalog_matches_no_display_handler_case_insensitively(
    preview_catalog: Path, tmp_path: Path
) -> None:
    write_handler(
        tmp_path,
        preview_catalog,
        extensions="JAR;Jad;",
        fields=f"Icon=game-symbolic\nNoDisplay=true\nX-FPLinux-FileIcon={preview_catalog}\n",
    )
    check_handler(
        preview_catalog,
        tmp_path,
        "/card/archive/Game.JaR",
        found=True,
        icon="game-symbolic",
        program=preview_catalog,
    )
    check_handler(preview_catalog, tmp_path, "/card/archive.txt/Game", found=False)


@pytest.mark.parametrize(
    "program_case",
    [
        "missing",
        "relative",
        "arguments",
        "directory",
        "symlink",
        "non-executable",
        "group-writable",
        "world-writable",
    ],
)
def test_invalid_preview_program_preserves_installed_handler_icon(
    preview_catalog: Path, tmp_path: Path, program_case: str
) -> None:
    program = tmp_path / ("preview --mode" if program_case == "arguments" else "preview")
    value = program.name if program_case == "relative" else str(program)
    if program_case == "directory":
        program.mkdir()
    elif program_case == "symlink":
        program.symlink_to(preview_catalog)
    elif program_case != "missing":
        program.write_bytes(b"#!/bin/sh\nexit 0\n")
        modes = {"non-executable": 0o644, "group-writable": 0o775, "world-writable": 0o757}
        program.chmod(modes.get(program_case, 0o755))
    write_handler(
        tmp_path, preview_catalog, fields=f"Icon=game-symbolic\nX-FPLinux-FileIcon={value}\n"
    )
    check_handler(preview_catalog, tmp_path, "game.jar", found=True, icon="game-symbolic")


def test_invalid_icon_preserves_a_valid_preview_program(
    preview_catalog: Path, tmp_path: Path
) -> None:
    write_handler(
        tmp_path,
        preview_catalog,
        fields=f"Icon=../invalid\nX-FPLinux-FileIcon={preview_catalog}\n",
    )
    check_handler(preview_catalog, tmp_path, "game.jar", found=True, program=preview_catalog)


def test_preview_catalog_rejects_ambiguity_even_when_one_handler_has_no_icon(
    preview_catalog: Path, tmp_path: Path
) -> None:
    write_handler(tmp_path, preview_catalog)
    write_handler(
        tmp_path, preview_catalog, app_id="org.example.other", extensions="jar;vxp;", fields=""
    )
    check_handler(preview_catalog, tmp_path, "game.jar", found=False)
    check_handler(preview_catalog, tmp_path, "game.vxp", found=True)


def test_preview_catalog_ignores_hidden_launcher_and_untrusted_metadata(
    preview_catalog: Path, tmp_path: Path
) -> None:
    write_handler(tmp_path, preview_catalog)
    write_handler(tmp_path, preview_catalog, app_id="org.example.hidden", fields="Hidden=true\n")
    write_handler(tmp_path, preview_catalog, app_id="org.fplinux.launcher", mode="launcher")
    write_handler(tmp_path, preview_catalog, app_id="org.example.invalid", extensions="jar;*;")
    untrusted = write_handler(tmp_path, preview_catalog, app_id="org.example.untrusted")
    untrusted.chmod(0o666)
    check_handler(preview_catalog, tmp_path, "game.jar", found=True, icon="game-symbolic")
