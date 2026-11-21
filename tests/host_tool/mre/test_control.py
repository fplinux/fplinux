# SPDX-License-Identifier: GPL-2.0-only
"""MRE control records and engine reply routing over real pipes."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-mre"


@pytest.fixture(scope="module")
def control(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "mre-control") as directory:
        executable = directory / "control"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(APORT),
                str(ROOT / "tests/host_tool/mre/control.c"),
                str(APORT / "ipc/mre-control.c"),
            ],
            name="compile the MRE control codec and pipe harness",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario",
    ["encoding", "fragmented-read", "malformed", "partial-write", "viewer-reply"],
)
def test_mre_control_pipe_contract(control: Path, scenario: str) -> None:
    run_process(
        [str(control), scenario],
        name=f"MRE control pipe {scenario}",
        timeout=10,
        check=True,
    )
