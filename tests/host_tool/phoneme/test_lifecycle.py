# SPDX-License-Identifier: GPL-2.0-only
"""VM scheduling decisions use literal slice results and clock values on the host."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def lifecycle(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    port = ROOT / "alpine/aports/fplinux-phoneme"
    with c_build_directory(tmp_path_factory, "phoneme-lifecycle") as directory:
        executable = directory / "lifecycle"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{port / 'port/app'}",
                f"-I{port / 'vm'}",
                str(ROOT / "tests/host_tool/phoneme/lifecycle-test.c"),
                str(port / "port/app/fplinux-pm-lifecycle.c"),
            ],
            name="compile phoneME lifecycle with a separate host peer",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario",
    ["hidden-wakeup", "hide-deadline", "slice-wakeup", "close-budget", "close-completion"],
)
def test_vm_scheduler_obeys_visibility_wakeups_and_close_deadlines(
    lifecycle: Path, scenario: str
) -> None:
    result = run_process(
        [str(lifecycle), scenario], name=f"phoneME scheduler {scenario}", timeout=10
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "lifecycle: passed\n"
    assert result.stderr == ""
