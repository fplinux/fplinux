# SPDX-License-Identifier: GPL-2.0-only
"""Terminal drawing geometry through a controlled LVGL drawing boundary."""

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


@pytest.fixture(scope="module")
def terminal_renderer(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "terminal-render") as directory:
        executable = directory / "terminal-render"
        aport = ROOT / "alpine/aports/fplinux-terminal"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'tests/host_tool/gui/terminal-boundary'}",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{aport}",
                str(ROOT / "tests/host_tool/gui/terminal-render-test.c"),
                str(aport / "terminal-render.c"),
                str(aport / "terminal-engine.c"),
                str(aport / "terminal-input.c"),
                str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                str(ROOT / "lib/fplinux/fplinux-language.c"),
                "-ltsm",
                *gettext_link_flags(),
            ],
            name="link terminal renderer with a drawing recorder",
        )
        yield executable


@pytest.mark.parametrize("scenario", ["glyphs", "decorations", "preedit", "layouts"])
def test_terminal_draws_with_shared_font_inside_fixed_cells(
    terminal_renderer: Path, scenario: str
) -> None:
    result = run_process(
        [str(terminal_renderer), scenario],
        name=f"record terminal {scenario} drawing geometry",
        timeout=10,
        check=True,
    )
    assert result.stdout == f"terminal-render {scenario}: OK\n"
