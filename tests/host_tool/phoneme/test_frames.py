# SPDX-License-Identifier: GPL-2.0-only
"""Real phoneME shell and Wayland presentation with a controlled Java VM."""

from __future__ import annotations

import zipfile
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.host_tool.mre.menu_frames import (
    capture_menu_frames,
    check_settings_cross_scope,
    check_settings_roundtrip,
    check_settings_save_retry,
)
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def phoneme_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path, ui_icon_resources: Path
) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "phoneme-native") as directory:
        run_process(
            [
                "sh",
                str(ROOT / "tests/host_tool/phoneme/native/build.sh"),
                str(ROOT),
                str(directory),
                str(ui_host_inputs),
            ],
            name="build actual phoneME shell, UI runtime and controlled Java VM",
            timeout=180,
            check=True,
        )
        (directory / "icons").symlink_to(ui_icon_resources, target_is_directory=True)
        yield directory


@pytest.mark.parametrize("screen", [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)])
@pytest.mark.parametrize("locale", ["en", "ru"])
@pytest.mark.parametrize("theme", ["light", "dark"])
def test_actual_phoneme_menu_frames(
    phoneme_native: Path, tmp_path: Path, screen: tuple[int, int], locale: str, theme: str
) -> None:
    """The VM peer supplies a MIDlet; menu screens use production UI objects."""
    document = tmp_path / "example.jar"
    write_jar(document)
    capture_menu_frames(
        phoneme_native, tmp_path, screen, locale, theme, application="phoneme", document=document
    )


def write_jar(document: Path) -> None:
    with zipfile.ZipFile(document, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(
            "META-INF/MANIFEST.MF",
            "MIDlet-Name: Example suite\r\nMIDlet-1: Example, , example.Game\r\n",
        )
        archive.writestr("example/Game.class", b"\xca\xfe\xba\xbe")


def test_actual_phoneme_settings_survive_reopen_and_reset_to_global_values(
    phoneme_native: Path, tmp_path: Path
) -> None:
    document = tmp_path / "example.jar"
    write_jar(document)
    check_settings_roundtrip(phoneme_native, tmp_path, "phoneme", document)


@pytest.mark.parametrize("operation", ["file", "directory"])
def test_actual_phoneme_same_choice_retries_failed_save_and_clears_unsaved(
    phoneme_native: Path, tmp_path: Path, operation: str
) -> None:
    document = tmp_path / "example.jar"
    write_jar(document)
    check_settings_save_retry(phoneme_native, tmp_path, "phoneme", document, operation)


@pytest.mark.parametrize("failed_scope", ["program", "global"])
def test_actual_phoneme_success_in_other_scope_keeps_failed_scope_unsaved(
    phoneme_native: Path, tmp_path: Path, failed_scope: str
) -> None:
    document = tmp_path / "example.jar"
    write_jar(document)
    check_settings_cross_scope(phoneme_native, tmp_path, "phoneme", document, failed_scope)
