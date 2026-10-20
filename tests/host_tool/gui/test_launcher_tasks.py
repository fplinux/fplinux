# SPDX-License-Identifier: GPL-2.0-only
"""Production Tasks callbacks with controlled native widgets and session transport."""

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
APORT = ROOT / "alpine/aports/fplinux-launcher"


@pytest.fixture(scope="module")
def launcher_tasks(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the real Tasks owner with test-owned widget and IPC boundaries."""
    with c_build_directory(tmp_path_factory, "launcher-tasks") as directory:
        lvgl = directory / "lvgl"
        lvgl.mkdir()
        (lvgl / "lvgl.h").write_bytes((HERE / "notes_lvgl.h").read_bytes())
        executable = directory / "launcher-tasks"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(directory),
                "-I",
                str(ROOT / "include/fplinux"),
                "-I",
                str(APORT),
                str(HERE / "launcher-tasks-test.c"),
                str(APORT / "launcher-tasks.c"),
                *gettext_link_flags(),
            ],
            name="compile production launcher Tasks callbacks",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario", ["confirm", "resume", "states", "refresh", "icons", "interruptions", "errors"]
)
def test_tasks_callbacks_preserve_instance_identity_and_visible_actions(
    launcher_tasks: Path, scenario: str
) -> None:
    result = run_process(
        [str(launcher_tasks), scenario],
        name=f"launcher Tasks {scenario}",
        timeout=10,
        check=True,
    )
    assert result.stdout == "launcher-tasks: scenario passed\n"
