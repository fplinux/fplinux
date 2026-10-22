# SPDX-License-Identifier: GPL-2.0-only
"""Launcher ring callbacks against controlled native Alert and session boundaries."""

from __future__ import annotations

import shlex
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
CLOCKS = ROOT / "alpine/aports/fplinux-clocks"


@pytest.fixture(scope="module")
def launcher_clock(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the real ring owner and codecs; replace widgets and IPC only."""
    flags = run_process(
        ["pkg-config", "--cflags", "--libs", "json-c"],
        name="resolve launcher clock JSON dependency",
        timeout=10,
        check=True,
    )
    with c_build_directory(tmp_path_factory, "launcher-clock") as directory:
        lvgl = directory / "lvgl"
        lvgl.mkdir()
        (lvgl / "lvgl.h").write_bytes((HERE / "notes_lvgl.h").read_bytes())
        executable = directory / "launcher-clock"
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
                "-I",
                str(CLOCKS),
                str(HERE / "launcher-clock-test.c"),
                str(APORT / "launcher-clock.c"),
                str(CLOCKS / "clock-model.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                str(CLOCKS / "clock-wire.c"),
                *shlex.split(flags.stdout),
                *gettext_link_flags(),
            ],
            name="compile production launcher ring callbacks",
        )
        yield executable


@pytest.mark.parametrize("scenario", ["alarm", "timer", "stale", "errors", "visibility"])
def test_ring_actions_wait_for_owner_and_preserve_event_identity(
    launcher_clock: Path, scenario: str
) -> None:
    result = run_process(
        [str(launcher_clock), scenario],
        name=f"launcher ring {scenario}",
        timeout=10,
        check=True,
    )
    assert result.stdout == "launcher-clock: scenario passed\n"
