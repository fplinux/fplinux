# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for the Snake game model."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-snake"


@pytest.fixture(scope="module")
def snake_model(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Compile the real model as a separate translation unit."""
    with c_build_directory(tmp_path_factory, "snake-model") as directory:
        executable = directory / "snake-model"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(APORT),
                str(ROOT / "tests/host_tool/gui/snake_model.c"),
                str(APORT / "snake.c"),
            ],
            name="compile Snake model harness",
        )
        yield executable


def test_rectangular_gameplay_and_score_contract(snake_model: Path) -> None:
    """Pause, turns, growth, collisions, food and winning use model results."""
    result = run_process(
        [str(snake_model)], name="run Snake model harness", timeout=10, check=True
    )
    assert result.stdout == "snake-model: all checks passed\n"
