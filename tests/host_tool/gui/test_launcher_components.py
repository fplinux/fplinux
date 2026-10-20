# SPDX-License-Identifier: GPL-2.0-only
"""Host launcher components: catalogue metadata, geometry and launch frames."""

from __future__ import annotations

import gettext
import os
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-launcher"
INCLUDE = ROOT / "include/fplinux"


@pytest.fixture(scope="module")
def launcher_logic(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the production catalogue and drawing-independent helpers."""
    with c_build_directory(tmp_path_factory, "launcher-logic") as temporary:
        executable = temporary / "launcher-logic"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(APORT),
                "-I",
                str(INCLUDE),
                str(ROOT / "tests/host_tool/gui/launcher-logic-test.c"),
                str(APORT / "launcher-catalogue.c"),
                str(APORT / "launcher-layout.c"),
                str(APORT / "launcher-power.c"),
                str(APORT / "launcher-text.c"),
                str(ROOT / "lib/fplinux/fplinux-desktop-entry.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
            ],
            name="compile launcher component harness",
        )
        yield executable


@pytest.fixture(scope="module")
def launch_frame(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the production RGB565 descriptor producer."""
    with c_build_directory(tmp_path_factory, "launcher-frame") as temporary:
        executable = temporary / "launcher-frame"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(APORT),
                "-I",
                str(INCLUDE),
                str(ROOT / "tests/host_tool/gui/launcher-launch-test.c"),
                str(APORT / "launcher-launch.c"),
            ],
            name="compile launcher launch frame harness",
        )
        yield executable


@pytest.mark.parametrize("contract", ["text", "navigation", "geometry"])
def test_launcher_helpers(launcher_logic: Path, contract: str) -> None:
    """Notices, input navigation and screen containment use production helpers."""
    run_process(
        [str(launcher_logic), contract],
        name=f"launcher {contract} component",
        timeout=10,
        env={**os.environ, "TZ": "UTC0"},
        check=True,
    )


def test_catalogue_admits_localized_installed_entries(
    launcher_logic: Path, tmp_path: Path
) -> None:
    """Localized names survive bad icons; hidden and unsafe metadata stay absent."""
    entries = {
        "org.test.notes": "Name=Notes\nName[ru]=Заметки\nIcon=notes\nCategories=Office;\n",
        "org.test.bad-icon": "Name=Bad icon\nIcon=../notes\nCategories=Game;\n",
        "org.test.hidden": "Name=Hidden\nNoDisplay=true\n",
        "org.test.unsafe": "Name=Unsafe\n",
    }
    for app_id, fields in entries.items():
        path = tmp_path / f"{app_id}.desktop"
        path.write_text(
            f"[Desktop Entry]\nType=Application\nExec={launcher_logic}\n"
            f"X-FPLinux-Mode=integrated\n{fields}",
            encoding="utf-8",
        )
        path.chmod(0o666 if app_id == "org.test.unsafe" else 0o600)
    run_process(
        [str(launcher_logic), "catalogue", str(tmp_path)],
        name="launcher catalogue metadata component",
        timeout=10,
        check=True,
    )


def test_launch_frame_packs_rows_and_limits_rectangular_storage(launch_frame: Path) -> None:
    """Owned launch pixels omit padding and share the portrait/landscape byte bound."""
    run_process(
        [str(launch_frame)], name="launcher launch frame component", timeout=10, check=True
    )


def test_russian_catalogue_translates_selected_ui(tmp_path: Path) -> None:
    """Compiled gettext output provides lock, error and battery messages."""
    catalogue = tmp_path / "ru.mo"
    run_process(
        [
            "msgfmt",
            "--check",
            "--output-file",
            str(catalogue),
            str(APORT / "ru.po"),
        ],
        name="compile launcher Russian catalogue",
        timeout=30,
        check=True,
    )
    with catalogue.open("rb") as file:
        russian = gettext.GNUTranslations(file)
    assert russian.gettext("Press Unlock, then *") == "«Разблокировать», затем *"
    assert russian.gettext("No apps installed") == "Приложения не установлены"
    assert russian.gettext("App did not start") == "Приложение не запустилось"
    assert russian.gettext("Battery low") == "Низкий заряд"
    assert russian.gettext("Menu") == "Меню"
    assert russian.gettext("Tasks") == "Задачи"
    assert russian.gettext("No open apps") == "Нет открытых приложений"
